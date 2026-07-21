[center][size=5][b]F4SE Menu Framework[/b][/size]
[size=3]A shared in-game Mod Control Panel for Fallout 4[/size][/center]

[line]

[center][size=3]Ported from Skyrim SE with permission from [url=https://www.nexusmods.com/profile/SkyrimThiago]SkyrimThiago[/url].
If this mod helps you, please endorse the [url=https://www.nexusmods.com/skyrimspecialedition/mods/120352?tab=description]original SKSE Menu Framework[/url] as well.[/size][/center]

[line]

[size=4][b]What is this?[/b][/size]

A modern, ImGui-based settings menu that other F4SE mods can plug into. No custom hooks or input handling required on their end. Open it with a hotkey, a gamepad combo, or from the pause menu.

It also includes a [b]translation layer[/b] for existing [url=https://www.nexusmods.com/fallout4/mods/21497]Mod Configuration Menu[/url] mods, so most MCM pages can be used from one place.

[quote][i]From the original SKSE Menu Framework page:[/i]

This mod adds a new menu so that other mods can register their own ImGui graphical interfaces. They can also create their own windows and open them however they want. It is all already set up. Import ImGui and this mod into your plugin and start using them. There is no need to use hooks or input management.[/quote]

[line]

[size=4][b]Features[/b][/size]

[list]
[*][b]In-game keybinds:[/b] set keyboard and gamepad activation keys from the Settings window; the bound key is shown on startup[/*]
[*][b]Font selection:[/b] swap fonts from Settings[/*]
[*][b]Hotkey API:[/b] mod authors can register named keybinds with conflict detection[/*]
[*][b]Gamepad support:[/b] navigation, control hints, and button glyphs[/*]
[*][b]Pause menu entry:[/b] "F4SE FRAMEWORK" row in the ESC menu for quick access[/*]
[*][b]Fallout-inspired theme:[/b] styled to fit the game's HUD look[/*]
[*][b]Menu search:[/b] find pages and settings quickly[/*]
[*][b]MCM compatibility:[/b] load and edit most existing MCM mods in this menu (see notes below)[/*]
[*][b]MCM images:[/b] logos and UI art from mod SWFs (bitmaps, vector shapes, and simple timeline animations)[/*]
[*][b]FallUI HUD editor & Icon Library:[/b] native recreations for editing the [b]base[/b] FallUI HUD presets and Icon Library (see FallUI note below)[/*]
[/list]

[line]

[size=4][b]MCM Compatibility[/b][/size]

If you already use mods that ship MCM configs (`Data\MCM\Config\...\config.json`), this framework can show them under [b]MCM Mod Configs (Legacy)[/b].

[list]
[*]Works with settings, keybinds, buttons, dropdowns, images, and most scripted MCM actions[/*]
[*]Writes the same `Data\MCM\Settings\` files as the original MCM[/*]
[*]Optional coexistence if you keep [url=https://www.nexusmods.com/fallout4/mods/21497]Mod Configuration Menu[/url] installed as well (toggle in Settings)[/*]
[/list]

[b]FallUI HUD presets (important)[/b]

The built-in FallUI HUD editor and Icon Library let you edit the [b]base presets and UI[/b] that ship with FallUI. If you want to use [b]custom FallUI HUD preset mods[/b] (community layout packs beyond the base set), use the original [url=https://www.nexusmods.com/fallout4/mods/21497]Mod Configuration Menu[/url] for that instead. The translation layer does not fully support those custom preset workflows.

[line]

[size=4][b]Requirements[/b][/size]

[list]
[*][url=https://f4se.silverlock.org/]F4SE[/url][/*]
[*][url=https://www.nexusmods.com/fallout4/mods/47327]Address Library for F4SE Plugins[/url][/*]
[*]Fallout 4 [b]1.10.163[/b] (old-gen), [b]1.10.980 / 1.10.984[/b] (Next-Gen), or [b]1.11.x[/b] (current patches). One DLL supports all of them. Game Pass and VR are [b]not[/b] supported (no F4SE).[/*]
[/list]

[line]

[size=4][b]How to open the menu[/b][/size]

[list]
[*][b]Keyboard:[/b] default [b]][/b] (Bracket Right); change it in Settings or `F4SEMenuFramework.ini`[/*]
[*][b]Gamepad:[/b] configurable (default uses LB double-press style; see Settings / INI)[/*]
[*][b]Pause menu:[/b] with `F4SEFramework.swf` installed, select [b]F4SE FRAMEWORK[/b] from the ESC list[/*]
[/list]

[line]

[size=4][b]Compatibility notes[/b][/size]

[list]
[*][b]Nvidia Smooth Motion[/b] is incompatible. It breaks the DX11 hook used by this (and most other) ImGui overlays. For frame generation, use [url=https://www.nexusmods.com/fallout4/mods/98208]Frame Generation[/url] on Nexus instead.[/*]
[/list]

[line]

[size=4][b]For mod authors[/b][/size]

[size=3][b]Native ImGui menus (recommended for new mods)[/b][/size]

[list]
[*]Example plugin: [url=https://github.com/DCCStudios/F4SEMenuFrameworkExample]F4SE Menu Framework Example[/url][/*]
[*]API recipes & hotkeys: [url=https://github.com/DCCStudios/F4SEMenuFramework/blob/master/Usage.md]Usage.md[/url][/*]
[*]Full walkthrough: [url=https://github.com/DCCStudios/F4SEMenuFramework/blob/master/PLUGIN_DEVELOPMENT_GUIDE.md]Plugin Development Guide[/url][/*]
[*]Copy `F4SEMenuFramework.h` from the source release. No linking required (runtime `GetProcAddress`)[/*]
[*]Register pages on [b]kPostLoad[/b] / [b]kPostPostLoad[/b], not only during `F4SEPlugin_Load`[/*]
[/list]

[size=3][b]MCM JSON / Papyrus (legacy)[/b][/size]

Keep shipping a normal MCM package (`config.json`, optional `settings.ini` / `keybinds.json`). Players using this framework will see your pages under [b]MCM Mod Configs (Legacy)[/b]. No separate framework config format is required. Prefer native ImGui registration for new F4SE plugins.

[size=3][b]Useful ImGui resources[/b][/size]

[list]
[*][url=https://pthom.github.io/imgui_manual_online/manual/imgui_manual.html]ImGui code snippets[/url][/*]
[*][url=https://raa.is/ImStudio/]ImStudio[/url]: interactive ImGui layout editor ([url=https://github.com/Raais/ImStudio]source[/url])[/*]
[*][url=https://fontawesome.com/search?o=r&m=free]Font Awesome[/url]: icons supported by this mod (see also the [url=https://github.com/Thiago099/SKSE-Menu-Framework-SDK/blob/main/README.md#font-awesome]SKSE Menu Framework SDK notes[/url])[/*]
[/list]

Fonts and general design patterns from the SKSE version still apply. For a strong example of ImGui styled like vanilla Fallout 4 UI, see [b]Photo Mode[/b] by po3.

[line]

[size=4][b]Source[/b][/size]

[url=https://github.com/DCCStudios/F4SEMenuFramework]github.com/DCCStudios/F4SEMenuFramework[/url]

[line]

[size=2][i]This mod was developed with assistance of Claude Opus 4.6. If that bothers you, please don't use the mod.[/i][/size]
