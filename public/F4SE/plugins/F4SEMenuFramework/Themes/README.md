# Making your own F4SE Menu Framework theme

This folder holds the theme files that show up in the **Menu Style** dropdown
in the framework's Settings window. A theme is just a small JSON text file —
no coding, compiling, or special tools needed. This guide walks a complete
beginner through making one from scratch.

## The fastest way to start

1. Copy `classic.json` and rename the copy to something like `MyTheme.json`.
   `classic.json` is intentionally empty — it's the smallest possible valid
   theme, and the safest starting point.
2. Open your new file in any plain text editor (Notepad, Notepad++, VS Code —
   anything that saves plain `.txt`-style text, not Word).
3. In-game, open the framework's Settings window and pick your new theme
   from the **Menu Style** dropdown. It appears automatically — new files are
   picked up **without restarting the game**.
4. Edit the JSON, save the file, and switch back to the game. Within about a
   second the change appears automatically — you don't need to reselect it
   from the dropdown. Keep the game window and your editor open side by side
   and iterate like that.

That auto-refresh is "live reload": the framework rechecks this folder about
once a second while the Settings window is open, so adding, removing, or
editing a theme file just works. If you make a typo that breaks the JSON, the
framework quietly ignores the broken parts (or falls back to the built-in
dark theme) instead of crashing — so feel free to experiment.

## The two things a theme file can control

A theme file is a single JSON object with up to two sections. Both are
optional — leave either one out (or leave the whole file as `{}`) and the
framework just keeps its built-in dark-theme defaults for whatever you didn't
specify.

```json
{
  "WindowRounding": 4.0,
  "ImGuiCol": {
    "WindowBg": "#1A1A1AEE",
    "Text": "#FFFFFFFF"
  }
}
```

### 1. Shape settings (top level, optional)

These are plain numbers that control rounding, border thickness, and
padding — the "shape" of widgets rather than their color. You will rarely
need more than a handful of these. The most useful ones:

| Key | What it does |
|---|---|
| `WindowRounding` | Corner roundness of windows, in pixels. `0` = sharp corners. |
| `FrameRounding` | Corner roundness of buttons, sliders, checkboxes, etc. |
| `GrabRounding` | Corner roundness of slider/scrollbar "grab" handles. |
| `ScrollbarRounding` | Corner roundness of the scrollbar track. |
| `WindowBorderSize` | Window border thickness in pixels. `0` = no border. |
| `FrameBorderSize` | Border thickness around buttons/inputs. |
| `FramePadding` | `[x, y]` padding inside buttons and inputs. |
| `WindowPadding` | `[x, y]` padding inside windows. |
| `ItemSpacing` | `[x, y]` gap between consecutive widgets. |

Every other shape key ImGui supports also works here (`Alpha`,
`IndentSpacing`, `TabRounding`, etc.) — see `fallout4.json` in this folder
for a file that sets most of them, and use it as a reference for spelling
and value shape (single numbers vs. `[x, y]` pairs).

### 2. Colors (`"ImGuiCol"`, optional)

This is a map of ImGui color-slot names to colors, written as hex strings in
**`"#RRGGBBAA"`** order — red, green, blue, then **alpha (opacity) last**.
That trailing alpha pair is easy to miss if you're used to web/CSS hex colors
(which are usually `#RRGGBB` or `#AARRGGBB`) — get it backwards and a color
will look fully transparent or fully solid when you didn't intend that.

- `FF` = fully opaque, `00` = fully invisible, `80` ≈ 50% see-through.
- Example: `"WindowBg": "#101010CC"` = a near-black window background
  (`10,10,10`) that's mostly but not fully opaque (`CC` ≈ 80%).

You don't need to set every color — anything you leave out keeps the
built-in dark-theme value for that slot. Start with just the handful below
and add more only if you want to fine-tune further.

| Key | What it colors |
|---|---|
| `WindowBg` | The main background of windows. |
| `Text` | Regular text color. |
| `TextDisabled` | Grayed-out / disabled text. |
| `Border` | Window and frame border lines. |
| `Button` / `ButtonHovered` / `ButtonActive` | Button background, normal / mouse-over / clicked. |
| `Header` / `HeaderHovered` / `HeaderActive` | Selectable rows, tree nodes, collapsing headers. |
| `FrameBg` / `FrameBgHovered` / `FrameBgActive` | Background of sliders, checkboxes, dropdowns, text inputs. |
| `CheckMark` | The tick inside a checked checkbox. |
| `SliderGrab` / `SliderGrabActive` | The draggable slider handle. |
| `Tab` / `TabHovered` / `TabActive` | Settings-page tab colors, if the theme uses tabs. |
| `ScrollbarGrab` / `ScrollbarGrabHovered` / `ScrollbarGrabActive` | The draggable scrollbar thumb. |
| `TitleBg` / `TitleBgActive` | Window title bar background. |

For the **complete** list of valid color keys, open `fallout4.json` in this
same folder — it sets nearly all of them and doubles as a full reference for
exact spelling (the keys must match ImGui's names exactly, e.g.
`ButtonHovered`, not `Button_Hovered` or `buttonHovered`). Any key that's
misspelled is silently ignored rather than causing an error, so if a color
doesn't seem to apply, double check the spelling against `fallout4.json`
first.

## A worked example: a purple theme in 5 minutes

1. Copy `classic.json` → `Purple.json`.
2. Open it and replace the contents with:

   ```json
   {
     "WindowRounding": 6.0,
     "FrameRounding": 4.0,
     "ImGuiCol": {
       "WindowBg": "#1A0F2ECC",
       "Text": "#F0E6FFFF",
       "Border": "#B388FF80",
       "Button": "#6A3FA033",
       "ButtonHovered": "#6A3FA066",
       "ButtonActive": "#9C6FE6AA",
       "CheckMark": "#B388FFFF",
       "SliderGrab": "#6A3FA0AA",
       "SliderGrabActive": "#9C6FE6FF"
     }
   }
   ```
3. Save the file, switch to the game, and open Settings — `PURPLE` is
   already in the **Menu Style** dropdown. Select it.
4. Not quite right? Switch back to your editor, tweak a hex value, save,
   and switch back to the game — no reselecting needed, it updates live.

## Where these files live

Theme files live in this folder:
`Data/F4SE/Plugins/F4SEMenuFramework/Themes/`
next to the framework's font folder
(`Data/F4SE/Plugins/F4SEMenuFramework/Fonts/`, which works the same way —
drop in a `.ttf`/`.otf` file and it appears in the Settings **Font**
dropdown, live, with no restart needed either).

## Troubleshooting

- **My theme doesn't show up in the dropdown.** Check the file actually ends
  in `.json` (not `.json.txt`, which some editors add silently) and that
  it's directly inside this `Themes` folder, not a subfolder.
- **The dropdown shows my filename in ALL CAPS.** That's expected — the
  framework displays theme names uppercased. Name the file however you like
  (`Purple.json` is fine).
- **A color looks invisible / fully solid when I expected a mix.** Almost
  always the alpha (last two hex characters) is `00` or `FF` — see the
  color-format note above.
- **Nothing I change seems to apply.** Make sure the JSON is valid: every
  `{`, `[`, and `"` needs a matching closing character, and every entry
  except the last one in a list needs a trailing comma. A JSON validator
  (search "json validator" online, or most text editors flag this
  automatically) will catch this instantly. An invalid file is never applied
  — the framework falls back to defaults rather than crashing, but that also
  means your changes just won't appear until the syntax is fixed.
