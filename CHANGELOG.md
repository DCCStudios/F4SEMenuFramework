# Changelog

All notable changes to F4SE Menu Framework are documented in this file.

## [3.3.0] — 2026-07-19

Compatibility release: **one DLL now supports every mainstream Fallout 4 runtime** — Old-gen **1.10.163**, Next-Gen **1.10.980 / 1.10.984**, and the current **1.11.x** patch line (verified against address libraries up to 1.11.221). Requires the Address Library `version-*.bin` matching your game version, as before.

### Multi-runtime support

- **CommonLibF4 replaced** with the Dear-Modding multi-runtime fork (vendored under `extern/commonlibf4`, built through a CMake shim since upstream is xmake-only). Every `REL::ID` carries three slots — OG / NG / AE — selected at runtime from the game executable's file version.
- **Dual loader support**: the DLL exports both the classic `F4SEPlugin_Query` (OG F4SE 0.6.23) and the `F4SEPlugin_Version` structure (NG/AE F4SE 0.7.x), with the address-library-independence flags set so future 1.11.x patches load without a plugin update as long as a matching Address Library exists.
- **Version-independent hooks** wherever an Address Library ID doesn't exist across all runtimes:
  - `ClipCursor` is now hooked by walking the game module's import table by name instead of Address Library ID 641385 — identical target on OG, and works on NG/AE where that ID was never published.
  - `D3D11CreateDeviceAndSwapChain`: OG and AE keep the proven call-site hook (IDs 224250 / 4492363); NG 1.10.980/984 — whose address libraries have no ID for the renderer-init function — falls back to patching the import pointer (IAT slot plus any cached copies the game stored in `.data` at static-init time).
- **Papyrus dispatch is ABI-aware per runtime**: NG/AE argument packs for `DispatchStaticCall` / `DispatchMethodCall` are plain C++ lambdas wrapped in `BSTThreadScrapFunction` (ABI-identical to those runtimes' `std::function`). OG 1.10.163 was built with the VS2013 toolchain whose `std::function` layout differs (32 bytes vs 64), so on OG the functor is constructed through the game's own engine helpers (IDs 445184 / 69733) — passing a modern `std::function` there read a garbage pointer and crashed (fixed a CTD when clicking MCM pages that fire external events, e.g. NAC X).
- The VM attached-scripts table, HUD color lookup, and all other game-struct accesses now go through the fork's runtime-aware definitions instead of hand-verified raw offsets.

### Localization

- **Language-aware translations**: the MCM translation layer now reads the game's `sLanguage:General` setting and loads `*_<lang>.txt` translation files with per-key English fallback, instead of always loading English. Applies to global translations, per-mod probing, and the FallUI HUD editor's own translation files.
- **Escape decoding**: literal `\n` / `\t` sequences in translation values are converted to real newlines/tabs, matching the game's Scaleform translator (fixes raw `\n` showing in descriptions).
- **Glyph coverage**: the glyph ranges matching the game language (Cyrillic, CJK, Thai, Vietnamese, Greek; Latin Extended always) are baked into the ImGui font atlas automatically — the manual `Enable*` INI flags remain as overrides.
- **Fallback fonts for missing scripts**: when the chosen UI font has no outlines for the needed script (e.g. Korean Hangul), a fallback font is merged in to fill only the missing glyphs — no more `?` boxes on CJK/Korean text, without changing the Latin look. Fallbacks are searched in `Data/F4SE/Plugins/Fonts/` first (ship `NotoSansKR-Regular.ttf`, `NotoSansSC-Regular.ttf`, `NotoSansJP-Regular.ttf`, `NotoSans-Regular.ttf` there to be self-contained, e.g. for Proton/Wine), then the Windows font directory (Malgun Gothic, Microsoft YaHei, Yu Gothic, Leelawadee, Segoe UI).
- **Fixed a CTD with CJK-capable fonts**: selecting a font that actually contains Chinese/Japanese/Korean glyphs could bake an atlas taller than D3D11's 16384-px texture limit; `CreateTexture2D` failed and the DX11 backend crashed on the null texture (verified against a Buffout 4 crash log, and reproduced offline: full Chinese at the three UI sizes hit 8192x16426). Fixed threefold: CJK glyphs now bake at 1x oversample (halves their atlas footprint; full Chinese fits at 8192x8352), the atlas build retries with progressively reduced coverage (full -> common Chinese subset -> no CJK) until it fits, and the backend tolerates texture-creation failure (blank text instead of a crash) as a last line of defense.
- **Legacy-codepage translation files** (e.g. Korean packs saved as ANSI/CP949 instead of UTF-8) are detected and converted to UTF-8 on load via the machine's ANSI codepage — previously every non-ASCII byte rendered as a U+FFFD replacement diamond.
- **Content-driven glyph coverage**: after loading, the resolved MCM text is scanned for non-Latin scripts (Hangul, kana, ideographs, Cyrillic, Thai, Greek, Vietnamese) and the font atlas is rebuilt with matching ranges when needed. This covers translation packs that ship non-Latin text in `*_en.txt` files on an `sLanguage=en` game, which the language setting alone can't announce.
- The left mouse button is excluded from hotkey capture (clicking is how the capture UI is operated, so it would instantly self-bind); left-button binds made in the real MCM still import and fire.

### Mouse button keybinds

- **MCM hotkey controls can bind mouse buttons** (left/right/middle/Mouse4/Mouse5), matching the real MCM. Capture accepts mouse presses, bindings dispatch from the game window's mouse messages, and Keybinds.json round-trips the real MCM's VK codes (1/2/4/5/6) so binds transfer between both systems.
- Internally, mouse buttons use the F4SE/Papyrus keycode convention (256–260) in the framework's keyboard binding space; INI persistence uses names `MOUSELEFT` … `MOUSE5`.
- Hotkey conflict detection now compares bindings per input device (a mouse code no longer false-conflicts with a gamepad code sharing the same number).

### Internal

- **Fixed the pause-menu button disappearing** after the CommonLibF4 fork migration: the fork's `GFx::Value::GetString()` returns `const char*` where the old library returned `std::string_view`, silently turning the "is this MainMenu.swf?" check into a pointer comparison that never matched — so the injection bailed for every movie. The comparison is content-based again.
- Logging moved to spdlog directly (the fork has no `F4SE::log` wrapper); the log file location and format are unchanged.
- Plugin version bumped to 3.3.0; `F4SE::Init` now allocates the trampoline (replaces `F4SE::AllocTrampoline`).

## [3.2.0] — 2026-07-17

Feature release: MCM image controls can now render Flash **vector art and timeline animations**, and FallUI's Flash-based in-MCM applications (the drag-and-drop **HUD layout editor** and the **Icon Library** preset manager) are recreated natively in ImGui with byte-identical persistence.

### MCM images — vector shapes and timeline animation (`SWFVectorMovie`)

- Previously only symbols with an **embedded raster bitmap** could be shown from `lib.swf` / `logo.swf`. A new CPU rasterizer (`SWFVectorMovie`) now handles symbols drawn with Flash **vector shapes** (`DefineShape1–4`: solid / gradient / bitmap fills and line strokes) and symbols **animated on the timeline** (per-frame `PlaceObject` matrices and color transforms).
- Resolution order per `(imageLibName, imageClassName)` — first hit wins, each step cached: embedded bitmap in `lib.swf` → bitmap in `logo.swf` → vector/timeline in `lib.swf` → vector/timeline in `logo.swf`. An empty/unknown class name falls back to the file's own main timeline (covers `logo.swf`-style files whose stage *is* the artwork).
- **Static** symbols are flattened once into a single supersampled RGBA texture; **animated** symbols replay their tweens as textured quads at the movie's frame rate. ActionScript-driven animation is out of scope (needs a Flash VM). Morph shapes, font glyph rendering, filters, non-normal blend modes, and clip masks are logged and skipped, never failing the whole movie.
- FallUI's landing-page **"intro" branding images** (`M8r.View.*Intro*` — every FallUI mod ships one) are rendered as **page backdrops**: fitted and centered behind the page's controls (via a background draw channel) instead of flowing inline, matching their role in the Scaleform MCM. The invisible `M8r.View.FixFileDropdown` AS3 shim renders nothing (its file-dropdown patch is implemented natively).

### FallUI HUD layout editor and Icon Library — native recreations

- The real FallUI mods embed full AS3 applications inside MCM `image` controls; those need a live Scaleform stage the translation layer doesn't provide. The two app controls (`M8r.Controller.FallUIHUD`, `M8r.View.FallUIIconLibrary`) are now **taken over by native ImGui recreations** (`FallUIHudEditor`).
- **HUD editor**: drag-and-drop canvas with every widget from the original catalog (ported from decompiled `HUDLayoutOptions.as`), per-widget floating edit panel with the full 1:1 option set (scale, rotation, visibility, colors, anchors, nudge arrows, and every widget-specific modifier), global settings, easy mode, profiles, and import/export from installed FallUI layout preset mods.
- **Persistence is format-identical** to the originals (verified against shipped presets): widget lines use FallUI's packed `on:<x>x<y>*<sx>*<sy>r<rot>:<k=v,...>` strings and editor/global settings use the M8r StringPacker `name;type;value;...` format, all in `Data/MCM/Settings/FallUIHUD.ini`. Layouts made here load in the Flash editor and vice versa, and FallUI's runtime HUD swf applies them unmodified.
- **Preview fidelity**: widget previews are drawn from each widget's live modifier values, using real art rasterized out of FallUI's `HUDMenu.swf` (crosshair, hit indicators, Vault Boy, compass/stealth chrome, icons — tinted with the game's current HUD color) plus procedural bars/text for elements Flash renders dynamically. 2D rotation and the full 3D options (`act3D`, `RX`/`RY`/`RZ`, perspective `LP`/`LPFOV`) are applied at the vertex level, so presets like **3D-Demo** preview correctly.
- **Icon Library** preset logic (item-sorter tag config selection) is recreated natively, writing the same setting the original wrote into FallUI's INI.

### MCM config parser — new control features

- **`dropdownFiles`** control type: a dropdown listing files from a directory. `valueOptions.path` is relative to the game root (e.g. `data\Interface\ItemSorter`), `valueOptions.mask` is a wildcard like `*.xml`; the stored value is the file name with extension.
- **Per-control `modName` override**: a control can read/write **another mod's** settings INI, matching real MCM behavior (FallUI's Icon Library writes `sItemSorterTagConfig` into `FallUI.ini` this way). All setters/getters and `OnMCMSettingChange` dispatch honor the override.
- **Typed action-param cast prefixes** (`"{i}42"`, `"{f}1.5"`, `"{b}1"`, `"{s}text"`, and `"{i}{value}"`-style casts of the control's value) are now decoded to their Papyrus types instead of being passed as strings, matching MCM's AS3 param parser. Fixes **NAC X**: its "Load <weather>" buttons pass `"{i}<n>"` to a `loadWeather(Int)` global, and a string argument made the VM's argument binding fail silently — the button did nothing.
- **`{value}` embedded in string params** (e.g. FallSouls' / Floating Damage's `"bConsole|{value}"`) is substituted with the control's stringified value, mirroring MCM's AS3 `replace()` behavior.
- **`CallExternalFunction` action type**: invokes a function the target F4SE plugin registered on the Scaleform `root.f4se.plugins.<plugin>` object (via F4SE's scaleform interface), exactly like MCM's AS3 does. Runs on the game UI thread against whichever always-loaded movie exposes the object (HUD during gameplay, pause menu when paused). Used by **FallSouls**, **Floating Damage**, **Crafting Highlight Fix**, **FM Extravaganza**, **Disable Companion Collision**.
- **Legacy `OnMCMOpen` / `OnMCMClose` external events now fire.** The real MCM sends these parameterless F4SE external events when its UI opens/closes; our layer declared the dispatchers but never called them. They now fire when the first translated MCM page comes on screen and when the user leaves MCM pages entirely (menu closed or navigated elsewhere), around the per-mod `OnMCMMenuOpen`/`OnMCMMenuClose` events. Native mods hang their "re-read settings INI" logic off `OnMCMClose` — **Baka Fullscreen Pip-Boy** (`UpdateSettings`), **GCM**, **Survival Configuration Menu**, **Power Armor to the People**, **IAA Backpack**, **NPCs Change Clothes** — so without it their settings edited in our pages only applied after a game restart.
- **`OnMCMMenuOpen` / `OnMCMMenuClose` events now match the shipped MCM.swf exactly.** Despite MCM.psc's doc comment claiming `(string modName)` parameters, the real MCM sends both the unfiltered and the `|ModName`-filtered variants with **no arguments** — and never sends an unfiltered `OnMCMMenuClose` at all. Our layer passed `modName` as an argument, and the Papyrus VM silently drops calls whose argument count doesn't match the handler, so zero-parameter handlers written against real MCM's actual behavior never fired. Fixes **Game Visuals Configuration Menu** (its `OnMCMMenuOpen()`/`OnMCMMenuClose()` handlers remove/reapply the pause-menu blur imagespace modifier around the menu).
- **Coexistence: keybind actions no longer double-fire.** With the real MCM installed and coexistence force-enabled, both MCM's own input handler and our translated keybinds dispatched the same action on one key press. Toggle hotkeys ran twice — **Fallout 4 Wheel Menu** and **Screen Archer Menu** opened and instantly closed (or failed to open at all). In coexistence the framework now registers keybinds display-only (still visible, rebindable, and synced through `Keybinds.json` / live sync) and leaves execution to the real MCM. Solo (no `mcm.dll`), our dispatch is unchanged.
- **Lenient JSON sanitizer** (`MCMJsonSanitizer.h`): real MCM parses configs with as3corelib's *non-strict* AS3 JSON decoder, and shipped mods depend on all three of its leniencies — comments, **trailing commas** (Workshop Plus `keybinds.json`), and **invalid `\` escapes inside strings** (Active Effects on HUD writes `Documents\My Games\...` in help text). Config and keybind files are now pre-sanitized to strict JSON before parsing (invalid escapes become literal backslashes, exactly as AS3 passes them through). Fixes 7 configs that previously failed to load at all: **Active Effects on HUD**, **ammoUI**, **Have a Seat**, **Jump Grunt**, **Kill Tips and Hit Indicator Sound**, **Wet Effects**, **Workshop Plus** (keybinds).

### UI fixes

- **Floating panel z-order**: the HUD editor's floating panels (edit panel, global settings, import dialog, widgets list) are kept above the main window every frame (previously they could fall behind it after first use, becoming unclickable). Active popup modals still take precedence.
- **Label clipping**: widget hover/selection titles and drag measurement text are clamped inside the canvas; titles near the top edge flip below the widget.
- Pause-menu row is now **"F4SE FRAMEWORK"** (all caps) to match the vanilla entries, which are uppercased localisation strings.

### Tooling (repo, not shipped)

- `swf/test/build_swftest.bat` + `swftest.cpp`: standalone validator that renders SWF symbols to PNGs (with `_dark` variants for white-on-transparent art) for offline verification of the vector rasterizer.
- `swf/test/build_falluitest.bat` + `falluitest.cpp`: headless harness for the FallUI editor — StringPacker/layout roundtrips, preset decoding, import/migration, and session behavior against real preset INIs, no game required.
- `swf/test/build_jsontest.bat` + `jsontest.cpp`: runs the lenient-JSON sanitizer over every MCM config/keybinds file under one or more modlist roots, verifying they all parse and that already-valid files are semantically untouched.

## [3.1.1] — 2026-07-17

Compatibility and hardening release: fixes for MCM mods whose settings or scripted actions didn't work through the translation layer, ABI-compatible natives for plugins that call MCM functions directly, crash guards, menu search, and gamepad UX polish.

### MCM translation layer — script execution fixes

- **`CallFunction` without `scriptName`** now falls back to `ScriptObject` (matches native MCM). Fixes buttons that dispatched to the attached form's own script — e.g. **MCM Weather Control**, **FastAddItemMenu**.
- **Live mirror into native MCM's setting store**: `SetModSetting*` writes are also dispatched through the registered `MCM.*` Papyrus natives, so native MCM's in-memory `SettingStore` (which only reads INIs at startup) sees changes immediately. Fixes **FOV Slider and Player Height**, **FallUI** suite settings not applying until restart.
- **New keybind action types**: `RunConsoleCommand` (via `RE::Console::ExecuteCommand`) and `SendEvent` (dispatches `OnControlDown` / `OnControlUp` external Papyrus events, with new key-up dispatch in `HotkeyManager`). Fixes **PhotoMode**, **Mod Switch Framework** hotkeys.
- **JSON comments tolerated** in `config.json` / `keybinds.json` (`nlohmann::json` now parses with `ignore_comments`), matching native MCM's lenient JsonCpp parser. Fixes **Jump Grunt**, **Elzee Recoil Shake** failing to load.

### Native-caller ABI compatibility (crash fixes)

- **F4SE-layout-compatible native functions**: some plugins (verified: **LooksMenuTempScroll**) bypass the Papyrus VM, read the raw callback pointer at offset 0x50 of the registered `IFunction`, and call it directly. CommonLibF4's `std::function`-based natives put a vtable pointer there → instant crash. The MCM natives are now registered through a custom `F4SECompatNativeFunction` that keeps a raw C shim at the f4se offset while still supporting normal VM dispatch.
- **Correct hidden-pointer return convention** for `GetModSettingString`: f4se's `BSFixedString` return goes through a caller-allocated slot (RCX), not RAX. Verified against the caller's disassembly; a register-return shim left the slot uninitialized → garbage pointer dereference.
- **SEH crash guards on every raw shim**: a direct caller with yet another ABI assumption now gets a safe "missing setting" default instead of crashing the game; the fault is logged once per (function, calling DLL) with module name and offset.

### Menu search

- **Left panel**: fuzzy search (substring or in-order subsequence, per token) across the whole tree — mod groups *and* nested pages; matching branches auto-expand.
- **Right panel**: new settings search box on MCM translated pages, filtering controls by label, help text, and id; section headers keep matching results in context.

### Gamepad UX

- **Welcome banner** now also shows the gamepad activation binding and its mode (e.g. "Hold [DPAD_UP] on your controller") when a controller is connected.
- **Close-button leak fixed**: dismissing the menu with B/Circle no longer opens the Pip-Boy. The XInput hook keeps buttons/triggers hidden from the game after a window closes until the controller is fully released once (sticks pass through so movement resumes instantly).
- **Default gamepad activation is now Hold + D-pad Up** (code defaults and shipping INI).

### Docs

- `README.md`, `PLUGIN_DEVELOPMENT_GUIDE.md`, `Usage.md` updated for the hotkey API and MCM translation layer; renamed the pinned group to **"MCM Mod Configs (Legacy)"**.
- `F4SE_Plugin_Development_Reference.md` §39.5 extended with the verified f4se native-function ABI details and the SEH defense pattern.

## [3.1.0] — 2026-07-15

Major feature release: MCM translation layer (drop-in coexistence with native MCM), pause-menu entry, full gamepad UX, and two-way live sync between the ImGui menus and the real Mod Configuration Menu.

### MCM translation layer (backwards compatibility)

- **Scan and translate** existing `Data/MCM/Config/<Mod>/` packages (`config.json`, `settings.ini`, `keybinds.json`) into native ImGui pages under a pinned **"MCM Mod Configs (Legacy)"** group at the top of the menu list.
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
