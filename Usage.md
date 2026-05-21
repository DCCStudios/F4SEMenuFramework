
import the F4SEMenuFramework.h into your header file, i reccomend you creating a UI.cpp/UI.h 
```cpp
#include "F4SEMenuFramework.h"
```

Define the render function for your menu entry. Here is an example:

```cpp
void __stdcall UI::Example1::Render() {
    ImGui::InputScalar("form id", ImGuiDataType_U32, &AddFormId, NULL, NULL, "%08X");

    if (ImGui::Button("Search")) {
        LookupForm();
    }

    if (AddBoundObject) {
        ImGui::Text("How much %s would you like to add?", AddBoundObject->GetName());
        ImGui::SliderInt("number", &Configuration::Example1::Number, 1, 100);
        if (ImGui::Button("Add")) {
            auto player = RE::PlayerCharacter::GetSingleton()->As<RE::TESObjectREFR>();
            player->AddObjectToContainer(AddBoundObject, nullptr, Configuration::Example1::Number, nullptr);
        }
    } else {
        ImGui::Text("Form not found");
    }
}
```

You should create a function in order to register your menu entries:

You should check if the user has the menu framework installed before doing anything in the register function

```cpp
if (!F4SEMenuFramework::IsInstalled()) {
    return;
}
```

Before registering any entries, you should choose a section for your menu to be in. It is recommended that you use your mod name as the section name to keep things organized

```cpp
F4SEMenuFramework::SetSection("<menu section name>");
```

Register your menu entry, it will be a page on the Mod Control Panel

```cpp
F4SEMenuFramework::AddSectionItem("Add Item", Example1::Render);
```
Here is what this example will look like (The style of the picture is outdated):

![image](https://github.com/Thiago099/SKSE-Menu-Framework-SDK/assets/66787043/8ebcd191-55a3-498b-bf36-0ca7337eff3a)

## Font Awesome

Header file
```cpp
namespace Example4 {
	inline std::string TitleText = "This is an " + FontAwesome::UnicodeToUtf8(0xf2b4) + " Font Awesome usage example";
	inline std::string Button1Text = FontAwesome::UnicodeToUtf8(0xf0e9) + " Umbrella";
	inline std::string Button2Text = FontAwesome::UnicodeToUtf8(0xf06e) + " Eye";
	void __stdcall Render();
}
```
cpp file

```cpp
void __stdcall UI::Example4::Render() {
    FontAwesome::PushBrands();
    ImGui::Text(TitleText.c_str());
    FontAwesome::Pop();

    FontAwesome::PushSolid();
    ImGui::Button(Button1Text.c_str());
    FontAwesome::Pop();

    ImGui::SameLine();

    FontAwesome::PushRegular();
    ImGui::Button(Button2Text.c_str());
    FontAwesome::Pop();
}
```
Here is what this example will look like:

![image](https://github.com/Thiago099/SKSE-Menu-Framework-SDK/assets/66787043/c3b7a913-fbb9-41be-ae38-d4c9efa8e2b3)


You can browse icons and get the Unicode IDs from the [Font Awesome](https://fontawesome.com/search?o=r&m=free) website
![image](https://github.com/Thiago099/SKSE-Menu-Framework-SDK/assets/66787043/ec5f14f1-5658-4f6e-8e60-2342f47f078e)



## Creating your own windows

Define your window render function

```cpp
void __stdcall UI::Example2::RenderWindow() {
    auto viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2{0.5f, 0.5f});
    ImGui::SetNextWindowSize(ImVec2{viewport->Size.x * 0.4f, viewport->Size.y * 0.4f}, ImGuiCond_Appearing);
    ImGui::Begin("My First Tool##MenuEntiryFromMod",nullptr, ImGuiWindowFlags_MenuBar); // If two mods have the same window name, and they open at the same time.
                                                                                         // The window content will be merged, is good practice to add ##ModName after the window name.
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open..", "Ctrl+O")) { /* Do stuff */
            }
            if (ImGui::MenuItem("Save", "Ctrl+S")) { /* Do stuff */
            }
            if (ImGui::MenuItem("Close", "Ctrl+W")) {
                ExampleWindow->IsOpen = false;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    if (ImGui::Button("Close Window")) {
        ExampleWindow->IsOpen = false;
    }
    ImGui::End();
}
```

Register your window. The register method will return an object with which you can set the `IsOpen` property from anywhere to open and close your window

```cpp
UI::Example2::ExampleWindow = F4SEMenuFramework::AddWindow(Example2::RenderWindow);
UI::Example2::ExampleWindow->IsOpen = true; 
```

Here is the example header

```cpp
namespace Example2{
void __stdcall RenderWindow();
inline MENU_WINDOW ExampleWindow;
}
```

Here is what your window will look like:

![image](https://github.com/Thiago099/SKSE-Menu-Framework-SDK/assets/66787043/c301cc1b-d435-47ad-9bdc-a635fa385986)

## Plugin Hotkey API

The framework provides a lightweight hotkey system that dispatches keyboard events via its WndProc hook and persists bindings to the `[Hotkeys]` section of `F4SEMenuFramework.ini`. Your plugin does not need its own WndProc hook or input handling — just register a callback.

### Registering a hotkey

```cpp
#include "F4SEMenuFramework.h"

void __stdcall MyToggleCallback() {
    // Called when the user presses the bound key (only while no blocking menu is open).
    MyOverlay::Toggle();
}

void RegisterMyHotkeys() {
    if (!F4SEMenuFramework::IsInstalled()) return;

    // "MyMod.ToggleOverlay" is a unique string id for this hotkey.
    // 0x3C is the default DIK scan code (F2).
    // If the user has already rebound it in F4SEMenuFramework.ini, that binding takes precedence.
    F4SEMenuFramework::Hotkeys::Register("MyMod.ToggleOverlay", 0x3C, MyToggleCallback);
}
```

Call `RegisterMyHotkeys()` during your plugin's `F4SEPlugin_Load` or on `kPostPostLoad` messaging.

### Registering multiple hotkeys

A mod can register as many hotkeys as it needs. Call `Register` once per action, each with a **unique string id** (use a mod prefix like `"MyMod.ActionName"` so ids stay distinct across plugins). Each registration gets its own handle, callback, and INI entry.

```cpp
void __stdcall OnToggleOverlay() { /* ... */ }
void __stdcall OnOpenDebug()     { /* ... */ }
void __stdcall OnQuickSave()     { /* ... */ }

void RegisterMyHotkeys() {
    if (!F4SEMenuFramework::IsInstalled()) return;

    F4SEMenuFramework::Hotkeys::Register("MyMod.ToggleOverlay", 0x3C, OnToggleOverlay); // F2
    F4SEMenuFramework::Hotkeys::Register("MyMod.OpenDebug",     0x58, OnOpenDebug);     // F12
    F4SEMenuFramework::Hotkeys::Register("MyMod.QuickSave",     0x43, OnQuickSave);     // F9
}
```

Re-registering the **same id** does not create a second hotkey — it updates the callback on the existing entry and returns the same handle.

### Querying the current binding

If your mod has its own settings UI and wants to display the current key:

```cpp
unsigned int currentScanCode = F4SEMenuFramework::Hotkeys::GetBinding("MyMod.ToggleOverlay");
// Convert to a display name using your own key name table or the framework's GetToggleKeyName pattern.
```

### Rebinding from your own UI

```cpp
// User selected a new key (e.g. from an ImGui combo or "press a key" prompt):
unsigned int newScanCode = 0x43; // F9
F4SEMenuFramework::Hotkeys::SetBinding("MyMod.ToggleOverlay", newScanCode);
// This automatically persists to F4SEMenuFramework.ini.
// If another mod already uses F9, a warning notification is shown on-screen.
```

### Checking for conflicts before rebinding

You can proactively check if a scan code is already in use before committing the rebind:

```cpp
unsigned int candidateKey = 0x43; // F9
if (F4SEMenuFramework::Hotkeys::HasConflict(candidateKey, "MyMod.ToggleOverlay")) {
    // Show a confirmation dialog in your UI, e.g.:
    // "F9 is already used by another mod. Bind anyway?"
} else {
    F4SEMenuFramework::Hotkeys::SetBinding("MyMod.ToggleOverlay", candidateKey);
}
```

Even without this check, `SetBinding` will still display a brief warning popup (top-right corner) whenever a conflict is detected, so users are always informed.

### Unregistering

If you need to remove a hotkey (e.g. your overlay is destroyed):

```cpp
int64_t handle = F4SEMenuFramework::Hotkeys::Register("MyMod.ToggleOverlay", 0x3C, MyToggleCallback);
// ... later ...
F4SEMenuFramework::Hotkeys::Unregister(handle);
```

### Common DIK scan codes

| Key | Scan Code |
|-----|-----------|
| F1  | 0x3B      |
| F2  | 0x3C      |
| F3  | 0x3D      |
| F4  | 0x3E      |
| F5  | 0x3F      |
| F6  | 0x40      |
| F7  | 0x41      |
| F8  | 0x42      |
| F9  | 0x43      |
| F10 | 0x44      |
| F11 | 0x57      |
| F12 | 0x58      |
| HOME | 0xC7     |
| INSERT | 0xD2   |
| DELETE | 0xD3   |

### Behavior notes

- Each hotkey needs a **unique id**; use a mod prefix (e.g. `MyMod.ToggleOverlay`, `MyMod.OpenDebug`).
- Hotkeys only fire on **first key-down** (not held repeats).
- Hotkeys are **suppressed** while any blocking framework window is open (the game is "paused").
- Multiple hotkeys can share the same scan code — all matching callbacks will fire.
- The `[Hotkeys]` INI section uses key names (e.g. `F2`, `HOME`, `LEFTCONTROL`) not raw numbers.
