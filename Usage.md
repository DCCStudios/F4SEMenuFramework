# F4SE Menu Framework — Usage

Practical recipes for plugin authors. For a full project setup, see [PLUGIN_DEVELOPMENT_GUIDE.md](PLUGIN_DEVELOPMENT_GUIDE.md). For players and MCM JSON authors, see [README.md](README.md).

Copy `resources/F4SEMenuFramework.h` into your project and include it (a small `UI.h` / `UI.cpp` pair works well):

```cpp
#include "F4SEMenuFramework.h"
```

Always guard registration:

```cpp
if (!F4SEMenuFramework::IsInstalled()) {
    return;
}
```

Register menus from **`kPostLoad`** or **`kPostPostLoad`** so the framework DLL is already mapped regardless of `plugins.txt` order.

---

## Menu pages

Pick a section (usually your mod name), then add pages:

```cpp
F4SEMenuFramework::SetSection("My Mod");
F4SEMenuFramework::AddSectionItem("Add Item", Example1::Render);
```

Example render callback:

```cpp
void __stdcall UI::Example1::Render() {
    ImGuiMCP::InputScalar("form id", ImGuiMCP::ImGuiDataType_U32, &AddFormId, NULL, NULL, "%08X");

    if (ImGuiMCP::Button("Search")) {
        LookupForm();
    }

    if (AddBoundObject) {
        ImGuiMCP::Text("How much would you like to add?");
        ImGuiMCP::SliderInt("number", &Configuration::Example1::Number, 1, 100);
        if (ImGuiMCP::Button("Add")) {
            auto player = RE::PlayerCharacter::GetSingleton();
            // ... add items using CommonLibF4 APIs ...
        }
    } else {
        ImGuiMCP::Text("Form not found");
    }
}
```

Folder-style paths work: `"Folder/Subfolder/Item"`.

---

## Font Awesome

```cpp
namespace Example4 {
    inline std::string TitleText = "This is an " + FontAwesome::UnicodeToUtf8(0xf2b4) + " Font Awesome usage example";
    inline std::string Button1Text = FontAwesome::UnicodeToUtf8(0xf0e9) + " Umbrella";
    inline std::string Button2Text = FontAwesome::UnicodeToUtf8(0xf06e) + " Eye";
    void __stdcall Render();
}
```

```cpp
void __stdcall UI::Example4::Render() {
    FontAwesome::PushBrands();
    ImGuiMCP::Text(TitleText.c_str());
    FontAwesome::Pop();

    FontAwesome::PushSolid();
    ImGuiMCP::Button(Button1Text.c_str());
    FontAwesome::Pop();

    ImGuiMCP::SameLine();

    FontAwesome::PushRegular();
    ImGuiMCP::Button(Button2Text.c_str());
    FontAwesome::Pop();
}
```

Browse free icons at [fontawesome.com/search](https://fontawesome.com/search?o=r&m=free) and use the Unicode codepoint with `FontAwesome::UnicodeToUtf8`.

---

## Creating your own windows

```cpp
void __stdcall UI::Example2::RenderWindow() {
    auto viewport = ImGuiMCP::GetMainViewport();
    ImGuiMCP::SetNextWindowPos(
        ImGuiMCP::ImVec2{viewport->Pos.x + viewport->Size.x * 0.5f,
                         viewport->Pos.y + viewport->Size.y * 0.5f},
        ImGuiMCP::ImGuiCond_Appearing,
        ImGuiMCP::ImVec2{0.5f, 0.5f});
    ImGuiMCP::SetNextWindowSize(
        ImGuiMCP::ImVec2{viewport->Size.x * 0.4f, viewport->Size.y * 0.4f},
        ImGuiMCP::ImGuiCond_Appearing);

    // Always add ##YourModName so two mods with the same title don't merge windows.
    ImGuiMCP::Begin("My First Tool##MyModName", nullptr, ImGuiMCP::ImGuiWindowFlags_MenuBar);

    if (ImGuiMCP::BeginMenuBar()) {
        if (ImGuiMCP::BeginMenu("File")) {
            if (ImGuiMCP::MenuItem("Close", "Ctrl+W")) {
                ExampleWindow->IsOpen = false;
            }
            ImGuiMCP::EndMenu();
        }
        ImGuiMCP::EndMenuBar();
    }

    if (ImGuiMCP::Button("Close Window")) {
        ExampleWindow->IsOpen = false;
    }
    ImGuiMCP::End();
}
```

```cpp
// true (default) = blocks game input while open
UI::Example2::ExampleWindow = F4SEMenuFramework::AddWindow(Example2::RenderWindow, true);
UI::Example2::ExampleWindow->IsOpen = true;
```

```cpp
namespace Example2 {
    void __stdcall RenderWindow();
    inline MENU_WINDOW ExampleWindow;
}
```

Pass `false` as the second argument for a non-blocking gameplay overlay (stays up after the Mod Control Panel closes). See the development guide §7.

---

## Plugin Hotkey API

The framework owns keyboard (WndProc) and gamepad (XInput poll) dispatch and persists bindings in the `[Hotkeys]` section of `F4SEMenuFramework.ini`. Your plugin does **not** need its own WndProc hook for simple toggle keys.

### Register a keyboard hotkey

```cpp
#include "F4SEMenuFramework.h"

void __stdcall MyToggleCallback() {
    // Fires on first key-down while no blocking framework window is open.
    MyOverlay::Toggle();
}

void RegisterMyHotkeys() {
    if (!F4SEMenuFramework::IsInstalled()) return;

    // Unique string id + default DIK scan code (0x3C = F2).
    // If the user already rebound this id in the INI, that value wins.
    F4SEMenuFramework::Hotkeys::Register("MyMod.ToggleOverlay", 0x3C, MyToggleCallback);
}
```

Call `RegisterMyHotkeys()` from the same **`kPostLoad` / `kPostPostLoad`** path as menu registration (after `IsInstalled()` can succeed).

### Register several hotkeys

Each action needs its own **unique id**. Prefer a mod prefix (`MyMod.…`).

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

Re-registering the **same id** updates the callback and returns the same handle (it does not create a second hotkey).

### Gamepad hotkeys

```cpp
// Config codes match the framework Settings gamepad list:
//   A=4096, B=8192, X=16384, Y=32768, LB=256, RB=512, LT=9, RT=10, …
F4SEMenuFramework::Hotkeys::RegisterGamepad("MyMod.ToggleOverlay.Pad", 4096, MyToggleCallback);

if (F4SEMenuFramework::IsControllerConnected()) {
    // optional UX: show pad-specific hints in your UI
}
```

Gamepad bindings are stored in `[Hotkeys]` with names like `A` / `LB` (not DIK names).

### Query / rebind from your own UI

```cpp
unsigned int current = F4SEMenuFramework::Hotkeys::GetBinding("MyMod.ToggleOverlay");

unsigned int candidate = 0x43; // F9
if (F4SEMenuFramework::Hotkeys::HasConflict(candidate, "MyMod.ToggleOverlay")) {
    // Ask the user in your UI first, or let SetBinding show the framework dialog.
}

F4SEMenuFramework::Hotkeys::SetBinding("MyMod.ToggleOverlay", candidate);
// Persists to F4SEMenuFramework.ini when applied.
```

**Conflict behavior (important):** if the new code is already used by another registered hotkey, `SetBinding` does **not** apply immediately. The framework opens a **confirmation dialog**; the binding is written only if the user confirms. `HasConflict` lets you warn earlier in your own UI. Unbinding with scan code `0` never conflicts.

### Unregister

```cpp
int64_t handle = F4SEMenuFramework::Hotkeys::Register("MyMod.ToggleOverlay", 0x3C, MyToggleCallback);
// ...
F4SEMenuFramework::Hotkeys::Unregister(handle);
```

### Common DIK scan codes

| Key | Scan code | Key | Scan code |
|-----|-----------|-----|-----------|
| F1 | `0x3B` | F7 | `0x41` |
| F2 | `0x3C` | F8 | `0x42` |
| F3 | `0x3D` | F9 | `0x43` |
| F4 | `0x3E` | F10 | `0x44` |
| F5 | `0x3F` | F11 | `0x57` |
| F6 | `0x40` | F12 | `0x58` |
| HOME | `0xC7` | INSERT | `0xD2` |
| DELETE | `0xD3` | LEFT CTRL | `0x1D` |

### Behavior notes

- Hotkeys fire on **first press** only (not key-repeat).
- Keyboard hotkeys are **suppressed** while any **blocking** framework window is open (`BlockUserInput == true`).
- Multiple hotkeys may share one scan code — **all** matching callbacks run.
- INI values use names (`F2`, `HOME`, `LB`), not raw numbers.
- Prefer this API over rolling your own `AddInputEvent` toggle unless you need full `RE::InputEvent` access.

---

## MCM translation layer

You usually **do not** call a C++ “MCM API” from your F4SE plugin for this. The framework scans disk and builds pages itself. This section explains how that interacts with your mod.

### If you ship an MCM config (JSON / Papyrus)

Keep the standard layout:

```
Data/MCM/Config/MyMod/config.json
Data/MCM/Config/MyMod/settings.ini      ; optional defaults
Data/MCM/Config/MyMod/keybinds.json     ; optional
```

Players with F4SE Menu Framework see your UI under **MCM Mod Configs (Legacy) → \<displayName\>**.  
Players with native MCM use the classic MCM. One package serves both.

Settings live in `Data/MCM/Settings/MyMod.ini` (created/updated when the user changes values). Do not put user saves only in `Config\...\settings.ini` — that file is for **defaults**.

### If your Papyrus script calls `MCM.*`

Use the normal MCM script API (`GetModSettingInt`, `SetModSettingFloat`, `RegisterForExternalEvent`, …). Sources for the stub script are in `public/Scripts/Source/User/MCM.psc`.

| Situation | Who provides `MCM.*` natives |
|-----------|------------------------------|
| `mcm.dll` **not** loaded | F4SE Menu Framework (if MCM compat is enabled) |
| `mcm.dll` **loaded** | Native MCM only (framework skips registering natives) |

`MCM.GetVersionCode()` from the framework reports **9** (aligned with MCM 1.40-style checks).

### Control and image support notes

- All standard MCM control types are supported, plus **`dropdownFiles`** (directory-listing dropdown: `valueOptions.path` relative to the game root, `valueOptions.mask` wildcard; stores the file name with extension) and the per-control **`modName`** override (the control targets another mod's settings INI, as in real MCM).
- **`image` controls** can use loose files (`imagePath`) or Flash library symbols (`imageLibName` / `imageClassName`). Symbol lookup tries embedded bitmaps first, then falls back to a built-in **vector-shape / timeline-animation rasterizer** — so gradient art, vector logos, and tween animations from `lib.swf` / `logo.swf` display without any bitmap export. Animation driven by ActionScript (not the timeline) cannot be replayed.
- **Action params** support MCM's typed cast prefixes (`"{i}42"`, `"{f}1.5"`, `"{b}1"`, `"{s}text"`, `"{i}{value}"` etc.) in `config.json` and `keybinds.json`; they are passed to Papyrus with the correct types. `"{value}"` embedded in longer strings is substituted as text.
- **All MCM action types** work, including `CallExternalFunction` (Scaleform functions F4SE plugins register on `root.f4se.plugins.<plugin>`), used by mods like FallSouls and Floating Damage.
- Full-page branding images (`M8r.View.*Intro*` classes) are drawn as **page backdrops** behind the controls, like in the Scaleform MCM.
- FallUI's embedded Flash apps — the **HUD layout editor** (`M8r.Controller.FallUIHUD`) and **Icon Library** (`M8r.View.FallUIIconLibrary`) — are recreated natively in ImGui. They read and write the exact same `Data/MCM/Settings/FallUIHUD.ini` formats as the originals, so layouts and presets round-trip with real FallUI.

### If you are a C++ plugin author

- Prefer the framework’s own `AddSectionItem` pages for native ImGui UX.
- Or ship an MCM `config.json` and let the translation layer host it — useful for supporting players who already know MCM tooling.
- Do **not** assume translated pages exist: users can disable `[MCMCompat] Enabled`, or native MCM may be present with coexistence off.

### Player toggles (also in framework Settings)

```ini
[MCMCompat]
Enabled = true
MCMCompatWhenNativePresent = false
```

Changing these in the UI requires a **game restart** before pages are (re)scanned.

### Coexistence with native MCM

When both are installed and `MCMCompatWhenNativePresent = true`:

- Duplicate menus are possible (classic MCM + **MCM Mod Configs (Legacy)**).
- Settings files are shared; the framework reloads INIs when its overlay opens.
- Hotkey rebinds can sync with the running MCM while the pause menu movie is loaded (opening via the pause-menu **F4SE Framework** button keeps that movie under the overlay).

When coexistence is **off** (default with `mcm.dll` present), the framework does not register translated MCM pages — native MCM remains the single UI.

---

## Textures

```cpp
ImTextureID tex = F4SEMenuFramework::LoadTexture("Data\\F4SE\\Plugins\\MyMod\\icon.svg", {64, 64});
ImTextureID tex2 = F4SEMenuFramework::LoadTexture("Data\\F4SE\\Plugins\\MyMod\\photo.png");
F4SEMenuFramework::DisposeTexture("Data\\F4SE\\Plugins\\MyMod\\icon.svg");
```

Paths are relative to the game root (`Fallout4.exe`). PNG, JPG, and SVG are supported; results are cached.

---

## Menu open / close events

```cpp
void __stdcall OnMenuEvent(F4SEMenuFramework::Events::Type type) {
    if (type == F4SEMenuFramework::Events::kOpenMenu)  { /* overlay opened */ }
    if (type == F4SEMenuFramework::Events::kCloseMenu) { /* overlay closed */ }
}

int64_t id = F4SEMenuFramework::Events::Register(OnMenuEvent);
// F4SEMenuFramework::Events::Unregister(id);
```

Also available: `kBeforeRender`, `kAfterRender`, and `Events::RegisterPriority(callback, priority)`.

---

## Toggle key display name

```cpp
const char* keyName = F4SEMenuFramework::GetToggleKeyName(); // e.g. "BRACKETRIGHT"
```

This is the **framework menu** toggle (from INI / Settings), not your plugin hotkeys.
