# F4SE Menu Framework 3

A shared in-game **Mod Control Panel** for Fallout 4, rendered with [Dear ImGui](https://github.com/ocornut/imgui) over DirectX 11.

Port of [SKSE Menu Framework 3](https://github.com/QTR-Modding/SKSE-Menu-Framework-3) by QTR-Modding, adapted for **Fallout 4** with [F4SE](https://f4se.silverlock.org/) and [CommonLibF4](https://github.com/Ryan-rsm-McKenzie/CommonLibF4).

Any F4SE plugin can register menu pages, popout windows, HUD overlays, input callbacks, and hotkeys — without shipping its own renderer. Version **3.1** also translates existing **Mod Configuration Menu (MCM)** configs into ImGui pages and can provide the MCM Papyrus API when native MCM is not installed.

---

## Who is this for?

| You are… | Start here |
|----------|------------|
| A **player** installing the mod | [For players](#for-players) |
| An **MCM mod author** (JSON / Papyrus only) | [MCM translation layer](#mcm-translation-layer) — usually nothing to change |
| An **F4SE plugin author** (C++) | [Quick start](#quick-start-f4se-plugin-authors) → [Usage.md](Usage.md) → [PLUGIN_DEVELOPMENT_GUIDE.md](PLUGIN_DEVELOPMENT_GUIDE.md) |

---

## Compatibility

| Game | Supported? |
|------|------------|
| Fallout 4 **1.10.163** (last pre–next-gen / “old-gen”) | Yes — primary target |
| Fallout 4 **1.10.162** | Should load (runtime floor in `F4SEPlugin_Query`) |
| Next-Gen (1.10.980 / 1.10.984), Game Pass 1.11.x, FO4 VR | **No** |

Requires **F4SE** and **Address Library for F4SE Plugins** matching your game version.

---

## For players

### Install

1. Install F4SE and Address Library for your game version.
2. Copy the framework files into `Data\F4SE\Plugins\` (or enable the mod in MO2/Vortex).
3. For the pause-menu button, also place `F4SEFramework.swf` in `Data\Interface\` (same folder convention as MCM’s SWFs).

### Opening the menu

- **Keyboard:** toggle key from `F4SEMenuFramework.ini` (default shipped value is **`]` / `BRACKETRIGHT`** — change it in the framework’s Settings window or the INI).
- **Gamepad:** configurable (default style uses **LB** with double-press — see INI / Settings).
- **Pause menu:** with `F4SEFramework.swf` installed, an **"F4SE Framework"** row appears in the ESC pause list and opens the overlay on top of the pause menu.

### MCM mods you already use

If you have mods that ship `Data\MCM\Config\<ModName>\config.json`:

- With **only** this framework (no `mcm.dll`): their settings appear under **MCM Mod Configs (Legacy)** in the Mod Control Panel, and Papyrus `MCM.GetModSetting*` / `SetModSetting*` keep working if you also ship the stub `MCM.pex` from this project’s `public\Scripts\` folder.
- With **native MCM** (`mcm.dll`) installed: by default the framework **does not** show duplicate MCM pages (native MCM stays authoritative). You can force coexistence in the framework Settings → **Load MCM Mod Configs (Legacy) Even With MCM Installed** (requires restart).

FallUI users: the **FallUI HUD** drag-and-drop layout editor and the **Icon Library** work here too — they are recreated natively (no Flash), and layouts/presets are fully interchangeable with the original MCM versions.

See [CHANGELOG.md](CHANGELOG.md) for details (3.2: vector/animated MCM images, native FallUI editor; 3.1: live hotkey sync, settings safety fixes, gamepad UX).

---

## Quick start (F4SE plugin authors)

Copy `resources/F4SEMenuFramework.h` into your project. **No linking** — the header uses `GetProcAddress` at runtime.

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

**Important:** call `UI::Register()` from F4SE messaging **`kPostLoad`** or **`kPostPostLoad`**, not only from `F4SEPlugin_Load`. Plugins load in `plugins.txt` order; if your DLL loads before `F4SEMenuFramework.dll`, `IsInstalled()` is false during your `Load` and registration silently does nothing.

`IsInstalled()` checks that the DLL is **loaded in the process** (`GetModuleHandleW`), not that a file exists under `Data\` relative to the working directory (MO2 often breaks path checks).

Full walkthrough: [PLUGIN_DEVELOPMENT_GUIDE.md](PLUGIN_DEVELOPMENT_GUIDE.md)  
API recipes (windows, Font Awesome, hotkeys, MCM notes): [Usage.md](Usage.md)

---

## Hotkey API (plugin authors)

Register named hotkeys; the framework dispatches them and saves bindings under `[Hotkeys]` in `F4SEMenuFramework.ini`.

```cpp
void __stdcall OnToggleOverlay() { /* ... */ }

void RegisterMyHotkeys() {
    if (!F4SEMenuFramework::IsInstalled()) return;

    // Unique id + default DIK scan code (0x3C = F2)
    F4SEMenuFramework::Hotkeys::Register("MyMod.ToggleOverlay", 0x3C, OnToggleOverlay);

    // Optional: gamepad (4096 = A, same codes as framework gamepad settings)
    F4SEMenuFramework::Hotkeys::RegisterGamepad("MyMod.ToggleOverlay.Pad", 4096, OnToggleOverlay);
}
```

Also available: `GetBinding`, `SetBinding`, `HasConflict`, `Unregister`, `IsControllerConnected()`.  
Details, DIK table, and conflict behavior: [Usage.md — Plugin Hotkey API](Usage.md#plugin-hotkey-api).

---

## MCM translation layer

### What it does

At game data ready, the framework can:

1. Scan `Data\MCM\Config\*\config.json` (and related `settings.ini` / `keybinds.json`).
2. Build ImGui pages under **MCM Mod Configs (Legacy)**.
3. Read/write `Data\MCM\Settings\<ModName>.ini` (same files native MCM uses).
4. If **`mcm.dll` is not loaded**, register the standard **MCM** Papyrus natives so scripts keep working.
5. Optionally coexist with native MCM (settings/hotkey two-way sync when enabled).
6. Render MCM `image` controls from mod SWFs — embedded bitmaps, **vector art, and timeline animations** (built-in rasterizer; no Flash player needed).
7. Recreate FallUI's Flash-based **HUD layout editor** and **Icon Library** natively in ImGui, with layouts that round-trip byte-for-byte with the originals.

### For MCM mod authors (no C++ required)

Keep shipping a normal MCM package:

```
Data/MCM/Config/MyMod/config.json
Data/MCM/Config/MyMod/settings.ini      (optional defaults)
Data/MCM/Config/MyMod/keybinds.json     (optional)
```

Players with this framework see your menu under **MCM Mod Configs (Legacy)**. Players with native MCM use the classic MCM UI. You do not need a separate “framework” config format.

Papyrus continues to use the usual API (`MCM.GetModSettingFloat`, `SetModSettingBool`, `RegisterForExternalEvent("OnMCMSettingChange|MyMod", ...)`, etc.). Stub sources live in `public/Scripts/`.

### Player / advanced settings (`F4SEMenuFramework.ini`)

```ini
[MCMCompat]
Enabled = true
MCMCompatWhenNativePresent = false
```

| Key | Default | Meaning |
|-----|---------|---------|
| `Enabled` | `true` | Master switch for the translation layer (restart required when toggled in UI). |
| `MCMCompatWhenNativePresent` | `false` | If `true` and `mcm.dll` is loaded, show translated pages alongside native MCM. |

These toggles are also in the framework’s in-game Settings window.

### Coexistence notes (when both are installed)

- Native MCM remains the Papyrus native provider (`mcm.dll` wins registration).
- With coexistence on, both UIs write the same settings INIs; the framework reloads from disk when its overlay opens.
- Hotkeys can sync both ways via MCM’s pause-menu Scaleform object when the pause menu is open (opening from the pause-menu button keeps that movie loaded under the overlay).

More detail: [Usage.md — MCM translation layer](Usage.md#mcm-translation-layer) and [CHANGELOG.md](CHANGELOG.md).

---

## API overview (consumer header)

Copy from `resources/F4SEMenuFramework.h`.

| Area | Entry points |
|------|----------------|
| Install check | `F4SEMenuFramework::IsInstalled()` |
| Menu pages | `SetSection`, `AddSectionItem` |
| Windows | `AddWindow(renderFn, pauseGame = true)` → `MENU_WINDOW` (`IsOpen`, `BlockUserInput`) |
| HUD | `AddHudElement` |
| Input | `AddInputEvent` |
| Hotkeys | `Hotkeys::Register`, `RegisterGamepad`, `GetBinding`, `SetBinding`, `HasConflict`, `Unregister` |
| Gamepad | `IsControllerConnected()` |
| Textures | `LoadTexture`, `DisposeTexture` |
| Events | `Events::Register` / `RegisterPriority` / `Unregister` (`kOpenMenu`, `kCloseMenu`, …) |
| Toggle key name | `GetToggleKeyName()` |
| Drawing | `ImGuiMCP::…` (full ImGui API) |
| Icons | `FontAwesome::PushSolid` / `PushRegular` / `PushBrands` / `Pop` |

---

## Building this repository

### Prerequisites

- Visual Studio 2022  
- CMake 3.21+  
- [vcpkg](https://github.com/microsoft/vcpkg) (`VCPKG_ROOT`)  
- CommonLibF4 (this workspace expects it under `../PluginTemplate/CommonLibF4`)

### Environment variables

| Variable | Purpose |
|----------|---------|
| `VCPKG_ROOT` | vcpkg install |
| `FALLOUT4_FOLDER` | Optional: copy DLL to `Data\F4SE\Plugins\` after build |
| `FALLOUT4_MODS_FOLDER` | Optional: MO2 mods folder deploy |

### Configure & build

```powershell
cmake --preset release
cmake --build build/release --config Release
# or: cmake --preset relwithdebuginfo && cmake --build build/relwithdebinfo --config RelWithDebInfo
```

Output: `Compile/F4SE/Plugins/F4SEMenuFramework.dll`

Pause-menu SWF: see `swf/build.ps1` and `swf/src/F4SEFrameworkPause.as`.

---

## Runtime files

| Path | Purpose |
|------|---------|
| `Data\F4SE\Plugins\F4SEMenuFramework.dll` | Framework |
| `Data\F4SE\Plugins\F4SEMenuFramework.ini` | Toggle keys, fonts, MCM compat, gamepad glyph style |
| `Data\F4SE\Plugins\F4SEMenuFrameworkStrings.json` | UI strings |
| `Data\F4SE\Plugins\F4SEMenuFrameworkThemes\*.json` | Themes |
| `Data\F4SE\Plugins\Fonts\*.ttf` | Fonts referenced by INI |
| `Data\F4SE\Plugins\F4SEMenuFramework\Gamepad\*.png` | Button glyphs for the hint bar |
| `Data\Interface\F4SEFramework.swf` | Pause-menu **"F4SE Framework"** button |
| `Data\Scripts\MCM.pex` (+ source) | Optional when native MCM is **not** installed |

---

## Docs map

| Doc | Audience |
|-----|----------|
| [README.md](README.md) (this file) | Overview |
| [Usage.md](Usage.md) | Copy-paste recipes: menus, windows, hotkeys, MCM |
| [PLUGIN_DEVELOPMENT_GUIDE.md](PLUGIN_DEVELOPMENT_GUIDE.md) | Full plugin setup from scratch |
| [CHANGELOG.md](CHANGELOG.md) | Release notes (3.1.0+) |
| [PLUGIN_DEVELOPMENT_GUIDE.md](PLUGIN_DEVELOPMENT_GUIDE.md) §16 | Framework hook internals (maintainers) |

---

## Credits

- **[QTR-Modding](https://github.com/QTR-Modding)** — original [SKSE Menu Framework 3](https://github.com/QTR-Modding/SKSE-Menu-Framework-3)
- **[Ryan McKenzie](https://github.com/Ryan-rsm-McKenzie)** — CommonLibF4
- **[ocornut](https://github.com/ocornut/imgui)** — Dear ImGui
- **[P-K-0](https://github.com/P-K-0/Shadow-Boost-FO4)** / **[jarari](https://github.com/jarari/GunMover)** — D3D11 / ImGui reference patterns
- MCM config format and Scaleform patterns — community MCM / f4mcm ecosystem
