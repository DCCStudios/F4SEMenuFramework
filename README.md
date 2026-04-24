# F4SE Menu Framework 3

A port of [SKSE Menu Framework 3](https://github.com/QTR-Modding/SKSE-Menu-Framework-3) by QTR-Modding, adapted for **Fallout 4** using [F4SE](https://f4se.silverlock.org/) and [CommonLibF4](https://github.com/Ryan-rsm-McKenzie/CommonLibF4).

Provides a shared in-game Mod Control Panel rendered with [Dear ImGui](https://github.com/ocornut/imgui) via a DirectX 11 overlay. Any F4SE plugin can register menu sections, popout windows, HUD overlays, and input callbacks without shipping its own rendering infrastructure.

---

## Features

- **Mod Control Panel** — a unified, searchable settings menu shared across all plugins that use the framework
- **Popout windows** — pausing or non-pausing ImGui windows opened from within the control panel or independently
- **HUD overlays** — always-on foreground draw-list elements rendered every frame
- **Input callbacks** — intercept and optionally block raw game input events
- **Themes** — JSON theme files placed in `Data\F4SE\Plugins\F4SEMenuFrameworkThemes\`
- **Translations** — UTF-8 string overrides via `Data\F4SE\Plugins\F4SEMenuFrameworkStrings.json`
- **Textures** — load SVG, DDS, PNG, and other image formats as ImGui textures
- **Font Awesome** — solid, regular, and brands icon fonts included

---

## Key Differences from SKSE Menu Framework 3

| Area | SKSE version | F4SE version |
|---|---|---|
| Script extender | SKSE64 | F4SE |
| CommonLib | CommonLibSSE-NG | CommonLibF4 |
| Plugin entry | `SKSEPluginLoad` | `F4SEPlugin_Query` / `F4SEPlugin_Load` |
| Namespace | `SKSEMenuFramework::` | `F4SEMenuFramework::` |
| Header | `SKSEMenuFramework.h` | `F4SEMenuFramework.h` |
| INI file | `SKSEMenuFramework.ini` | `F4SEMenuFramework.ini` |
| Plugin folder | `Data\SKSE\Plugins\` | `Data\F4SE\Plugins\` |
| D3D hook method | IAT (`D3D11CreateDeviceAndSwapChain`) | `write_call<5>` on call site (`REL::ID 224250 + 0x419`) |

---

## Quick Start (plugin developer)

Copy `resources/F4SEMenuFramework.h` into your plugin project. No linking is required — the header uses `GetProcAddress` at runtime.

```cpp
#include "F4SEMenuFramework.h"

void UI::Register() {
    if (!F4SEMenuFramework::IsInstalled()) {
        return;
    }

    F4SEMenuFramework::SetSection("My Mod");
    F4SEMenuFramework::AddSectionItem("Settings", MyMod::RenderSettings);
}
```

Call `UI::Register()` from your `F4SEPlugin_Load` or from a `kGameDataReady` messaging callback.

---

## API Reference

### Check installation

```cpp
bool F4SEMenuFramework::IsInstalled()
```

### Register a menu section

```cpp
F4SEMenuFramework::SetSection("My Mod Name");
F4SEMenuFramework::AddSectionItem("Page Title", MyRenderFunction);
```

### Open/close-aware windows

```cpp
// Pausing window (game freezes while open)
MENU_WINDOW MyWindow = F4SEMenuFramework::AddWindow(MyMod::RenderWindow);

// Non-pausing overlay (game runs normally)
MENU_WINDOW MyOverlay = F4SEMenuFramework::AddWindow(MyMod::RenderOverlay, false);

// Control visibility from anywhere
MyWindow->IsOpen = true;
```

Non-blocking windows survive main-menu close — set `IsOpen = true` at init time for a permanent HUD overlay.

### HUD foreground overlays

```cpp
F4SEMenuFramework::AddHudElement(MyMod::RenderHud);

void __stdcall MyMod::RenderHud() {
    if (F4SEMenuFramework::IsAnyBlockingWindowOpened()) return;
    auto* drawList = ImGuiMCP::GetForegroundDrawList();
    // draw with ImGuiMCP::ImDrawListManager helpers
}
```

### Input callbacks

```cpp
F4SEMenuFramework::AddInputEvent(MyMod::OnInput);

bool __stdcall MyMod::OnInput(RE::InputEvent* event) {
    if (*event->device == RE::INPUT_DEVICE::kKeyboard) {
        if (auto* btn = event->As<RE::ButtonEvent>()) {
            if (btn->idCode == 0x30 && btn->JustPressed()) { // DIK_B
                MyOverlay->IsOpen = !MyOverlay->IsOpen;
                return true; // block the event from reaching the game
            }
        }
    }
    return false;
}
```

### Get the configured toggle key name

```cpp
const char* keyName = F4SEMenuFramework::GetToggleKeyName(); // e.g. "BRACKETRIGHT"
```

### Textures

```cpp
ImTextureID tex = F4SEMenuFramework::LoadTexture("Data\\interface\\my-icon.svg", {64, 64});
ImTextureID tex2 = F4SEMenuFramework::LoadTexture("Data\\interface\\my-image.png");
F4SEMenuFramework::DisposeTexture("Data\\interface\\my-icon.svg");
```

### Events

```cpp
void __stdcall OnMenuEvent(F4SEMenuFramework::Model::EventType type) {
    if (type == F4SEMenuFramework::Model::EventType::kOpenMenu)  { /* menu opened */ }
    if (type == F4SEMenuFramework::Model::EventType::kCloseMenu) { /* menu closed */ }
}

auto* evt = new F4SEMenuFramework::Model::Event(OnMenuEvent);
// delete evt; to unsubscribe
```

---

## Building

### Prerequisites

- Visual Studio 2022
- [CMake](https://cmake.org/) 3.21+
- [vcpkg](https://github.com/microsoft/vcpkg) (set `VCPKG_ROOT` environment variable)
- [CommonLibF4](https://github.com/Ryan-rsm-McKenzie/CommonLibF4) cloned to `../PluginTemplate/CommonLibF4/CommonLibF4/`

### Environment variables

| Variable | Purpose |
|---|---|
| `VCPKG_ROOT` | Path to your vcpkg installation |
| `FALLOUT4_FOLDER` | Game root (e.g. `C:\Steam\steamapps\common\Fallout 4`) — DLL is copied to `Data\F4SE\Plugins\` on build |
| `FALLOUT4_MODS_FOLDER` | Optional MO2 mods folder for automatic deployment |

### Configure & build

```powershell
cmake --preset relwithdebinfo
cmake --build build/relwithdebinfo --config RelWithDebInfo
```

Output: `Compile/F4SE/Plugins/F4SEMenuFramework.dll`

---

## Runtime files

Place these in `Data\F4SE\Plugins\` alongside the DLL:

| File | Purpose |
|---|---|
| `F4SEMenuFramework.ini` | Toggle key, toggle mode, font sizes, style |
| `F4SEMenuFrameworkStrings.json` | UI string translations |
| `F4SEMenuFrameworkThemes\*.json` | Custom ImGui themes |
| `Fonts\*.ttf` | Font files referenced by the INI |

---

## Credits

- **[QTR-Modding](https://github.com/QTR-Modding)** — original [SKSE Menu Framework 3](https://github.com/QTR-Modding/SKSE-Menu-Framework-3), on which this port is based
- **[Ryan McKenzie](https://github.com/Ryan-rsm-McKenzie)** — CommonLibF4
- **[ocornut](https://github.com/ocornut/imgui)** — Dear ImGui
- **[P-K-0](https://github.com/P-K-0/Shadow-Boost-FO4)** — Shadow-Boost-FO4 (D3D11 / ImGui reference)
- **[jarari](https://github.com/jarari/GunMover)** — GunMover (D3D11 / ImGui reference)
