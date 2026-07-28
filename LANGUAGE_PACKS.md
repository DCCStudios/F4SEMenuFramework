# Making a language pack for a plugin

You do **not** need to be a programmer, and you do **not** need the plugin's source code. A language pack is a small JSON text file that maps English menu text to another language. The framework swaps it in automatically when the player's game language matches.

This works for any mod that shows settings through **F4SE Menu Framework** (native ImGui pages). You do not need the mod author to change anything.

---

## What you will make

A file like this:

```json
{
  "Enabled": "Activado",
  "Inertia Effects": "Efectos de inercia",
  "Active Preset:": "Ajuste activo:"
}
```

- The part on the **left** (the key) is the English text the mod already shows. Leave it alone.
- The part on the **right** (the value) is your translation. Edit only that.

Save the file as a language code, for example `es.json` for Spanish, `de.json` for German, `fr.json` for French.

---

## Step by step

### 1. Find the plugin's folder name

Open the framework menu and look at the mod's entry in the left list. The **top-level name** (for example `FP Gunplay Overhaul`) is usually the folder name you need.

If you are unsure, look next to the mod's DLL:

```
Data/F4SE/Plugins/SomeMod.dll
```

Then the language folder is either:

```
Data/F4SE/Plugins/<Name From The Menu>/Languages/
```

or, if that name does not exist as a folder yet, create it. Prefer the **menu section name** when the mod has one (it usually matches what you see in the tree).

### 2. Collect the English strings (capture mode)

The easiest way to get every string the mod shows:

1. Open `F4SEMenuFramework.ini` (next to `F4SEMenuFramework.dll`).
2. Add or edit this section:

```ini
[Localization]
AutoTranslate = true
CaptureStrings = true
```

3. Start the game, open the framework menu, and click through **every page** of the mod you want to translate (labels only appear if that page drew them).
4. Close the framework menu. The framework writes a starter file here:

```
Data/F4SE/Plugins/<Name>/Languages/captured_strings.json
```

5. Turn capture back off so it does not keep rewriting files:

```ini
[Localization]
AutoTranslate = true
CaptureStrings = false
```

### 3. Make your language file

1. Copy `captured_strings.json` and rename the copy to your language, for example `es.json`.
2. Open it in a plain text editor (Notepad, Notepad++, VS Code). Do not use Word.
3. Translate only the **right-hand** values. Keep the left-hand keys exactly as they are.
4. Save as **UTF-8** if your editor offers an encoding choice.

Example before:

```json
{
  "Enabled": "Enabled",
  "Inertia Effects": "Inertia Effects"
}
```

Example after (Spanish):

```json
{
  "Enabled": "Activado",
  "Inertia Effects": "Efectos de inercia"
}
```

### 4. Set the game language

The framework uses the same language as Fallout 4:

1. Open `Documents\My Games\Fallout4\Fallout4Custom.ini` (or `Fallout4.ini` if Custom is missing).
2. Under `[General]`, set `sLanguage=` to your code, for example `sLanguage=es`.

Common codes: `en`, `de`, `es`, `fr`, `it`, `ja`, `ko`, `pl`, `ptbr`, `ru`, `zh`.

Restart the game (or reload the modlist), open the mod's menu, and check that your text appears.

### 5. Ship it as a small mod

Your pack only needs the language files. Example layout for a Spanish pack:

```
Data/F4SE/Plugins/FP Gunplay Overhaul/Languages/es.json
```

Install it like any other mod (MO2 / Vortex). Put it **after** the original mod so your file wins if both ship a language file.

---

## Rules that keep things working

### Leave `%` codes alone

Some strings insert numbers or names, for example:

```json
"Moved: %.1f / %.0f units": "Movido: %.1f / %.0f unidades"
```

Keep every `%...` piece exactly as in the English version (same letters, same order). You can move words around, but you must not drop, add, or change those `%` codes. If you do, the framework ignores that one line and shows English instead. That is intentional: a bad `%` line can crash the game, so the framework refuses it.

Safe:

```json
"Moved: %.1f / %.0f units": "Movido: %.1f / %.0f unidades"
```

Not safe (missing a code):

```json
"Moved: %.1f / %.0f units": "Movido: %.1f unidades"
```

### Keys must match exactly

`"Enabled"` and `"enabled"` are different. Copy keys from the capture file; do not retype them from memory.

### Labels with `##` in them

If you see something like `"Enable##opt1"`, you only need the visible part as the key:

```json
"Enable": "Activar"
```

The framework keeps the `##...` part for itself (it is an internal ID, not player text).

### You do not need `en.json`

English comes from the mod itself. Your pack is usually just `es.json` (or whatever language). An `en.json` is optional and only needed if you want to override English wording.

---

## Checklist

1. Capture strings with `CaptureStrings = true`, then turn it off.
2. Copy the capture file to `es.json` (or your language code).
3. Translate values only; keep keys and `%` codes exact.
4. Put the file under `Data/F4SE/Plugins/<Mod Name>/Languages/`.
5. Match the game's `sLanguage` to that file name.
6. Test in game.

---

## If something does not translate

- Confirm the file path and name (`es.json`, not `spanish.json`).
- Confirm `sLanguage` matches that code.
- Confirm `AutoTranslate = true` in `F4SEMenuFramework.ini`.
- Confirm you opened every page during capture (new pages add new strings).
- Check `Documents\My Games\Fallout4\F4SE\F4SEMenuFramework.log` for lines about rejected format strings; those lines still show English until you fix the `%` codes.
- MCM-style menus that use `$MyMod_Key` text are a different system: see [PLUGIN_DEVELOPMENT_GUIDE.md](PLUGIN_DEVELOPMENT_GUIDE.md) section 12, "MCM packages".

---

## For mod authors (short)

You usually do nothing. Automatic translation already covers ImGui text your plugin draws through the framework. If you want ID-style keys (`"Option.Enable"`) or strings built outside ImGui, use the `Translate()` API documented in [PLUGIN_DEVELOPMENT_GUIDE.md](PLUGIN_DEVELOPMENT_GUIDE.md) section 12.
