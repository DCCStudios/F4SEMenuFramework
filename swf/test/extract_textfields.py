# Extracts named TextField metrics from an ffdec -swf2xml dump of HUDMenu.swf.
#
# FallUI's Flash editor derives option stdValues from the live TextFields
# (width/height/font size/align). Our native recreation must use the same
# numbers or its pack filter drops/keeps different modifiers than the
# original. This prints, per DefineSprite class, every named edit-text
# instance with its static metrics so FallUIHudEditor.cpp's TextFieldDefaults
# can be checked against ground truth.
#
# Usage: py extract_textfields.py _hudmenu.xml

import re
import sys
import xml.etree.ElementTree as ET

def main(path):
    tree = ET.parse(path)
    root = tree.getroot()

    # characterID -> edit text info
    edittexts = {}
    for item in root.iter("item"):
        if item.get("type") == "DefineEditTextTag":
            cid = item.get("characterID")
            bounds = item.find("bounds")
            w = (int(bounds.get("Xmax")) - int(bounds.get("Xmin"))) / 20.0
            h = (int(bounds.get("Ymax")) - int(bounds.get("Ymin"))) / 20.0
            txt = item.get("initialText") or ""
            m = re.search(r'size="(\d+)"', txt)
            if m:
                size = int(m.group(1))
            else:
                # No inline html size: the tag's fontHeight (twips) is the format size
                fh = item.get("fontHeight")
                size = int(fh) // 20 if fh else None
            m = re.search(r'align="(\w+)"', txt)
            align = m.group(1) if m else "left"
            edittexts[cid] = (w, h, size, align, item.get("autoSize"))

    # symbol class names: tag id -> class
    classes = {}
    for item in root.iter("item"):
        if item.get("type") == "SymbolClassTag":
            tags = [i.text for i in item.find("tags").iter("item")]
            names = [i.text for i in item.find("names").iter("item")]
            for t, n in zip(tags, names):
                classes[t] = n

    # sprite id -> [(instance name, edit text char id)]
    for item in root.iter("item"):
        if item.get("type") != "DefineSpriteTag":
            continue
        sid = item.get("spriteId")
        cls = classes.get(sid, "sprite_" + sid)
        rows = []
        sub = item.find("subTags")
        if sub is None:
            continue
        for place in sub.findall("item"):
            if not (place.get("type") or "").startswith("PlaceObject"):
                continue
            name = place.get("name")
            cid = place.get("characterId")
            if name and cid in edittexts:
                rows.append((name, edittexts[cid]))
        if rows:
            print(f"== {cls} (sprite {sid})")
            for name, (w, h, size, align, auto) in rows:
                print(f"   {name}: w={w:g} h={h:g} fontSize={size} align={align} autoSizeTag={auto}")

if __name__ == "__main__":
    main(sys.argv[1])
