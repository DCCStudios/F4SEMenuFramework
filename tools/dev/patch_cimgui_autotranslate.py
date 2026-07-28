#!/usr/bin/env python3
"""Routes label-bearing cimgui exports through AutoTranslate.

cimgui.cpp is generated code (the DLL's plugin-facing ImGui ABI). This script
rewrites the ImGui:: call inside selected exported functions so the string
parameters plugins pass are run through the AutoTranslate lookup first, e.g.

    return ImGui::Checkbox(label,v);
becomes
    return ImGui::Checkbox(AutoTranslate::Label(label),v);

Run from the repo root after regenerating cimgui.cpp:
    python tools/dev/patch_cimgui_autotranslate.py

Safety rules:
  - Replacements happen only on lines containing "ImGui::" or "self->"
    (never in signatures or va_start lines).
  - Every mapped parameter must be replaced exactly once per function;
    anything else aborts without writing.
  - Already-patched functions (body mentions AutoTranslate::) are skipped,
    so the script is idempotent.
"""

import re
import sys
from pathlib import Path

CIMGUI = Path(__file__).resolve().parents[2] / "src" / "cimgui.cpp"

INCLUDE_LINE = '#include "AutoTranslate.h"'

def L(p):  # plain widget label
    return (p, f"AutoTranslate::Label({p})")

def F(p):  # printf format string (specifier-guarded)
    return (p, f"AutoTranslate::Format({p})")

def R(p, end):  # (text, text_end) pair, only substituted when end is null
    return (p, f"AutoTranslate::Range({p},{end})")

def A(p, count):  # const char* const items[]
    return (p, f"AutoTranslate::LabelArray({p},{count})")

def Z(p):  # zero-separated items blob
    return (p, f"AutoTranslate::ZeroSeparated({p})")

# functionName -> list of (paramName, wrappedExpression)
PATCHES = {
    # ---- printf-format family (CTD-guarded substitution) ----
    "igText": [F("fmt")],
    "igTextV": [F("fmt")],
    "igTextColored": [F("fmt")],
    "igTextColoredV": [F("fmt")],
    "igTextDisabled": [F("fmt")],
    "igTextDisabledV": [F("fmt")],
    "igTextWrapped": [F("fmt")],
    "igTextWrappedV": [F("fmt")],
    "igLabelText": [L("label"), F("fmt")],
    "igLabelTextV": [L("label"), F("fmt")],
    "igBulletText": [F("fmt")],
    "igBulletTextV": [F("fmt")],
    "igSetTooltip": [F("fmt")],
    "igSetTooltipV": [F("fmt")],
    "igSetItemTooltip": [F("fmt")],
    "igSetItemTooltipV": [F("fmt")],
    "igTreeNode_StrStr": [F("fmt")],
    "igTreeNode_Ptr": [F("fmt")],
    "igTreeNodeV_Str": [F("fmt")],
    "igTreeNodeV_Ptr": [F("fmt")],
    "igTreeNodeEx_StrStr": [F("fmt")],
    "igTreeNodeEx_Ptr": [F("fmt")],
    "igTreeNodeExV_Str": [F("fmt")],
    "igTreeNodeExV_Ptr": [F("fmt")],
    # ---- plain-label family ----
    "igBegin": [L("name")],
    "igButton": [L("label")],
    "igSmallButton": [L("label")],
    "igTabItemButton": [L("label")],
    "igCheckbox": [L("label")],
    "igCheckboxFlags_IntPtr": [L("label")],
    "igCheckboxFlags_UintPtr": [L("label")],
    "igCheckboxFlags_S64Ptr": [L("label")],
    "igCheckboxFlags_U64Ptr": [L("label")],
    "igRadioButton_Bool": [L("label")],
    "igRadioButton_IntPtr": [L("label")],
    "igProgressBar": [L("overlay")],
    "igBeginCombo": [L("label"), L("preview_value")],
    "igCombo_FnStrPtr": [L("label")],
    "igSliderFloat": [L("label")],
    "igSliderFloat2": [L("label")],
    "igSliderFloat3": [L("label")],
    "igSliderFloat4": [L("label")],
    "igSliderInt": [L("label")],
    "igSliderInt2": [L("label")],
    "igSliderInt3": [L("label")],
    "igSliderInt4": [L("label")],
    "igSliderScalar": [L("label")],
    "igSliderScalarN": [L("label")],
    "igSliderAngle": [L("label")],
    "igVSliderFloat": [L("label")],
    "igVSliderInt": [L("label")],
    "igVSliderScalar": [L("label")],
    "igDragFloat": [L("label")],
    "igDragFloat2": [L("label")],
    "igDragFloat3": [L("label")],
    "igDragFloat4": [L("label")],
    "igDragFloatRange2": [L("label")],
    "igDragInt": [L("label")],
    "igDragInt2": [L("label")],
    "igDragInt3": [L("label")],
    "igDragInt4": [L("label")],
    "igDragIntRange2": [L("label")],
    "igDragScalar": [L("label")],
    "igDragScalarN": [L("label")],
    "igInputText": [L("label")],
    "igInputTextMultiline": [L("label")],
    "igInputTextWithHint": [L("label"), L("hint")],
    "igInputFloat": [L("label")],
    "igInputFloat2": [L("label")],
    "igInputFloat3": [L("label")],
    "igInputFloat4": [L("label")],
    "igInputInt": [L("label")],
    "igInputInt2": [L("label")],
    "igInputInt3": [L("label")],
    "igInputInt4": [L("label")],
    "igInputDouble": [L("label")],
    "igInputScalar": [L("label")],
    "igInputScalarN": [L("label")],
    "igColorEdit3": [L("label")],
    "igColorEdit4": [L("label")],
    "igColorPicker3": [L("label")],
    "igColorPicker4": [L("label")],
    "igColorButton": [L("desc_id")],
    "igTreeNode_Str": [L("label")],
    "igTreeNodeEx_Str": [L("label")],
    "igCollapsingHeader_TreeNodeFlags": [L("label")],
    "igCollapsingHeader_BoolPtr": [L("label")],
    "igSelectable_Bool": [L("label")],
    "igSelectable_BoolPtr": [L("label")],
    "igBeginListBox": [L("label")],
    "igListBox_FnStrPtr": [L("label")],
    "igMenuItem_Bool": [L("label")],
    "igMenuItem_BoolPtr": [L("label")],
    "igMenuItemEx": [L("label")],
    "igBeginMenu": [L("label")],
    "igBeginMenuEx": [L("label")],
    "igBeginTabItem": [L("label")],
    "igSeparatorText": [L("label")],
    "igValue_Bool": [L("prefix")],
    "igValue_Int": [L("prefix")],
    "igValue_Uint": [L("prefix")],
    "igValue_Float": [L("prefix")],
    "igTableSetupColumn": [L("label")],
    "igTableHeader": [L("label")],
    "igPlotLines_FloatPtr": [L("label"), L("overlay_text")],
    "igPlotLines_FnFloatPtr": [L("label"), L("overlay_text")],
    "igPlotHistogram_FloatPtr": [L("label"), L("overlay_text")],
    "igPlotHistogram_FnFloatPtr": [L("label"), L("overlay_text")],
    # ---- popup-name family (displayed AND used as a cross-call key, so
    #      every member must translate identically or popups stop opening) ----
    "igBeginPopupModal": [L("name")],
    "igBeginPopup": [L("str_id")],
    "igOpenPopup_Str": [L("str_id")],
    "igIsPopupOpen_Str": [L("str_id")],
    "igOpenPopupOnItemClick": [L("str_id")],
    "igBeginPopupContextItem": [L("str_id")],
    "igBeginPopupContextWindow": [L("str_id")],
    "igBeginPopupContextVoid": [L("str_id")],
    # ---- window-name family (same consistency requirement as popups) ----
    "igSetWindowFocus_Str": [L("name")],
    "igSetWindowCollapsed_Str": [L("name")],
    "igSetWindowPos_Str": [L("name")],
    "igSetWindowSize_Str": [L("name")],
    "igFindWindowByName": [L("name")],
    # ---- (text, text_end) family ----
    "igTextUnformatted": [R("text", "text_end")],
    "igCalcTextSize": [R("text", "text_end")],
    # ---- items arrays / blobs ----
    "igCombo_Str_arr": [L("label"), A("items", "items_count")],
    "igCombo_Str": [L("label"), Z("items_separated_by_zeros")],
    "igListBox_Str_arr": [L("label"), A("items", "items_count")],
    # ---- raw draw-list text (HUD overlays draw through these) ----
    "ImDrawList_AddText_Vec2": [R("text_begin", "text_end")],
    "ImDrawList_AddText_FontPtr": [R("text_begin", "text_end")],
}


def main() -> int:
    text = CIMGUI.read_text(encoding="utf-8")
    lines = text.splitlines(keepends=True)

    # Ensure the include is present (after the last include in the header
    # block at the top of the file).
    if INCLUDE_LINE not in text:
        for i, line in enumerate(lines[:80]):
            if line.startswith("#include"):
                last_inc = i
        lines.insert(last_inc + 1, INCLUDE_LINE + "\n")

    # Index function definition lines: "CIMGUI_API <ret> <name>(".
    sig_re = re.compile(r"^CIMGUI_API\s+[^(]*?\b([A-Za-z_][A-Za-z0-9_]*)\(")
    patched, skipped, errors = [], [], []
    seen = set()

    i = 0
    while i < len(lines):
        m = sig_re.match(lines[i])
        if not m or m.group(1) not in PATCHES:
            i += 1
            continue
        name = m.group(1)
        seen.add(name)
        # Body runs to the first line that is exactly "}".
        j = i + 1
        while j < len(lines) and lines[j].rstrip("\r\n") != "}":
            j += 1
        body = lines[i + 1 : j]

        if any("AutoTranslate::" in ln for ln in body):
            skipped.append(name)
            i = j + 1
            continue

        ok = True
        new_body = list(body)
        for param, expr in PATCHES[name]:
            count = 0
            for k, ln in enumerate(new_body):
                if "ImGui::" not in ln and "self->" not in ln:
                    continue
                pat = re.compile(rf"(?<![A-Za-z0-9_:]){re.escape(param)}(?![A-Za-z0-9_])")
                ln2, n = pat.subn(expr, ln)
                if n:
                    new_body[k] = ln2
                    count += n
            if count != 1:
                errors.append(f"{name}: param '{param}' replaced {count} times (expected 1)")
                ok = False
        if ok:
            lines[i + 1 : j] = new_body
            patched.append(name)
        i = j + 1

    missing = sorted(set(PATCHES) - seen)
    for name in missing:
        errors.append(f"{name}: function not found in cimgui.cpp")

    if errors:
        print("ABORTED, no changes written:")
        for e in errors:
            print("  " + e)
        return 1

    CIMGUI.write_text("".join(lines), encoding="utf-8")
    print(f"Patched {len(patched)} function(s), {len(skipped)} already patched.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
