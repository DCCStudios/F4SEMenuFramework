# Changelog

All notable changes to F4SE Menu Framework are documented in this file.

## [3.1.0] — 2026-07-15

Major feature release: MCM translation layer (drop-in coexistence with native MCM), pause-menu entry, full gamepad UX, and two-way live sync between the ImGui menus and the real Mod Configuration Menu.

### MCM translation layer (backwards compatibility)

- **Scan and translate** existing `Data/MCM/Config/<Mod>/` packages (`config.json`, `settings.ini`, `keybinds.json`) into native ImGui pages under a pinned **"MCM Mod Settings"** group at the top of the menu list.
- **Drop-in Papyrus API**: registers the same `MCM.*` natives (`GetModSetting*` / `SetModSetting*`, etc.) when the real `mcm.dll` is **not** loaded, so mods that call the MCM script API keep working. When native MCM is present, its natives take precedence and ours are skipped.
- **Coexistence mode**: when native MCM is installed, the translation layer can still show the same configs (gated by conflict check / force-enable). Conflict detection no longer falsely treats a plain pause menu as “MCM open”; it checks whether MCM’s config panel is actually visible (`MainPanel_mc.visible`).
- **Control coverage**: sliders, switchers, steppers, dropdowns, text inputs (string/float), hotkeys, images, spacers, sections, HTML text, and related action types (`CallFunction`, `CallGlobalFunction`, etc.).
- **Images from MCM configs**:
  - Loose files via `imagePath`.
  - Flash library symbols (`imageLibName` / `imageClassName`) extracted from mod SWFs (`lib.swf` / `logo.swf`) with a custom SWF bitmap parser (zlib / lossless / JPEG), then uploaded as D3D11 textures for ImGui.
- **Keybinds**: maps MCM `keybinds.json` into the framework `HotkeyManager` (VK ↔ DIK), persists through `Data/MCM/Settings/Keybinds.json` when acting as sole provider, and routes through native MCM when it is installed.

### Two-way sync with native MCM

- **Settings (native → framework)**: on overlay open, reloads layered `Data/MCM/Settings/<Mod>.ini` from disk and invalidates widget state so edits made in the real MCM appear immediately.
- **Keybinds (native → framework)**: pulls live bindings via `root.mcm.GetKeybind` on the pause-menu movie (file can be stale until game save).
- **Keybinds (framework → native)**: live push through `root.mcm.RemapKeybind` / `SetKeybind` / `ClearKeybind` so rebinds apply without a reload; UI shows pending / synced / failed. `Keybinds.json` remains the load-time fallback.
- Pause menu is **kept open** under the overlay (input blocked) so `root.mcm` stays available for instant sync when opening from the pause-menu button.

### Settings INI safety (critical fix)

- **Never write a UTF-8 BOM** to `Data/MCM/Settings/*.ini`. CSimpleIni’s default `SaveFile(..., a_bAddSignature=true)` prepends `EF BB BF`, which makes the **first INI section invisible** to Windows profile APIs (`GetPrivateProfileSectionNames`) — exactly how native MCM’s `SettingStore` reads files. Symptom: native MCM showed zeros / defaults while the translation layer still showed correct values (e.g. Persistent Volume Sliders muted).
- **Dirty-only flush**: only mods whose values actually changed are written; untouched files are never rewritten.
- **Startup repair**: strips BOMs from existing `Data/MCM/Settings/*.ini` so prior buggy writes are healed automatically.
- **Reload hardening**: failed disk reads keep the previous in-memory cache instead of replacing it with empty data that could later overwrite user settings.

### Pause menu integration

- Ships a small clean-room SWF (`F4SEFramework.swf`) injected into the pause menu with an **"F4SE Framework"** list entry (placed above native MCM when present).
- Selecting it opens the existing ImGui overlay **on top of** the pause menu (game stays paused; input is fully blocked from the list underneath).
- Build tooling: `swf/build.ps1` + Apache Flex `mxmlc` source under `swf/src/`.

### Gamepad support

- Full gamepad navigation for the framework UI (XInput), including shoulder-button navigation between sections.
- On-screen control hints with Xbox / PlayStation button glyphs (shipped under `public/F4SE/plugins/F4SEMenuFramework/Gamepad/`).
- XInput IAT hook suppresses gamepad input to the game and other mods while the menu is open (second pass at `kGameDataReady` for late-loaded modules).

### Framework / API

- Hotkey manager extensions for MCM mapping import, live import without write-back echo, and richer rebinding behavior.
- Texture loader: `GetTextureFromMemory` for RGBA buffers (SWF-extracted bitmaps).
- Config toggle for MCM compatibility.
- Optional MCM Papyrus stub script package under `public/Scripts/` for installs without native MCM.
- Dependency: `zlib` (SWF decompression); links `windowscodecs` / `ole32` for WIC JPEG decode.

### Compatibility

- Targets **Fallout 4 1.10.163** (last pre–next-gen / “old-gen” build) via classic F4SE + Address Library `REL::ID` hooks.
- Not supported: Next-Gen (1.10.980 / 1.10.984), Game Pass 1.11.x, or Fallout 4 VR (vendored CommonLibF4 ends at 1.10.163; no `F4SEPlugin_Version` export).

### Build / packaging

- Updated `CMakeLists.txt` for MCM sources, zlib, and WIC libs.
- SWF sources, glyph processing helper (`tools/process_glyphs.py`), and public asset tree included for packaging.

---

## [3.0.0] — prior

Initial public line: ImGui Mod Control Panel for F4SE, plugin registration API, themes, hotkey API for mod authors, settings blur fix, and related stability fixes. See git history for detail.
