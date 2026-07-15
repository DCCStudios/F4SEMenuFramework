"""
process_glyphs.py — one-shot asset pipeline for the gamepad button glyphs.

The source PNGs (1024x1024) were generated with a light-gray/white checkerboard
"transparency" pattern baked into the RGB pixels (the files have no real alpha
channel). This script:

  1. Detects checkerboard-ish pixels (light gray ~204 and white ~255, low
     saturation).
  2. Flood-fills from the image borders across ONLY checkerboard-ish pixels,
     so interior whites (e.g. the "LB" text or the white ring of a button)
     are preserved — only background connected to the border becomes
     transparent.
  3. Feathers the resulting alpha edge slightly (3x3 box blur on the mask
     boundary) to avoid jaggies.
  4. Downscales to 64x64 with Lanczos resampling.
  5. Writes RGBA PNGs into the repo's public payload folder so CMake's
     post-build step ships them under Data/F4SE/Plugins/F4SEMenuFramework/Gamepad/.

Run:  python tools/process_glyphs.py
"""

import os
from collections import deque

import numpy as np
from PIL import Image

SRC_DIR = r"C:\Users\rober\.cursor\projects\e-Fallout-4-Modding-F4SE\assets"
DST_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "public", "F4SE", "plugins", "F4SEMenuFramework", "Gamepad",
)

GLYPHS = [
    "xbox_a", "xbox_b", "xbox_x", "xbox_y", "xbox_lb", "xbox_rb",
    "ps_cross", "ps_circle", "ps_square", "ps_triangle", "ps_l1", "ps_r1",
    "pad_dpad", "pad_lstick",
]

OUT_SIZE = 64


def checkerboard_mask(rgb: np.ndarray) -> np.ndarray:
    """True where a pixel *could* be part of the baked checkerboard.

    Checkerboard squares are neutral (R≈G≈B) and bright: light gray ~200-215
    or near-white ~245-255. Require low channel spread so colored glyph pixels
    never qualify.
    """
    r = rgb[..., 0].astype(np.int16)
    g = rgb[..., 1].astype(np.int16)
    b = rgb[..., 2].astype(np.int16)
    mx = np.maximum(np.maximum(r, g), b)
    mn = np.minimum(np.minimum(r, g), b)
    neutral = (mx - mn) <= 14          # nearly gray
    bright = mn >= 185                 # light gray or white
    return neutral & bright


def flood_from_border(candidate: np.ndarray) -> np.ndarray:
    """BFS flood fill from all border pixels across candidate-True pixels.

    Returns a bool mask of background pixels (connected to the border).
    Uses a scanline-ish BFS with numpy row slicing for speed at 1024^2.
    """
    h, w = candidate.shape
    bg = np.zeros_like(candidate, dtype=bool)
    q = deque()

    # Seed the queue with every candidate pixel on the border.
    for x in range(w):
        if candidate[0, x]:
            q.append((0, x))
        if candidate[h - 1, x]:
            q.append((h - 1, x))
    for y in range(h):
        if candidate[y, 0]:
            q.append((y, 0))
        if candidate[y, w - 1]:
            q.append((y, w - 1))

    for y, x in q:
        bg[y, x] = True

    while q:
        y, x = q.popleft()
        for ny, nx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
            if 0 <= ny < h and 0 <= nx < w and candidate[ny, nx] and not bg[ny, nx]:
                bg[ny, nx] = True
                q.append((ny, nx))
    return bg


def box_blur(a: np.ndarray) -> np.ndarray:
    """Cheap 3x3 box blur for edge feathering (float 0..1)."""
    p = np.pad(a, 1, mode="edge")
    return (
        p[:-2, :-2] + p[:-2, 1:-1] + p[:-2, 2:] +
        p[1:-1, :-2] + p[1:-1, 1:-1] + p[1:-1, 2:] +
        p[2:, :-2] + p[2:, 1:-1] + p[2:, 2:]
    ) / 9.0


def process(name: str) -> None:
    src = os.path.join(SRC_DIR, name + ".png")
    img = Image.open(src).convert("RGB")
    rgb = np.asarray(img)

    cand = checkerboard_mask(rgb)
    bg = flood_from_border(cand)

    # Alpha: 0 on background, 255 elsewhere; feather the boundary a bit.
    alpha = np.where(bg, 0.0, 1.0)
    alpha = box_blur(box_blur(alpha))

    rgba = np.dstack([rgb, (alpha * 255.0 + 0.5).astype(np.uint8)])
    out = Image.fromarray(rgba, "RGBA").resize((OUT_SIZE, OUT_SIZE), Image.LANCZOS)

    os.makedirs(DST_DIR, exist_ok=True)
    dst = os.path.join(DST_DIR, name + ".png")
    out.save(dst)

    pct = 100.0 * bg.mean()
    print(f"{name}: background {pct:.1f}% keyed out -> {dst}")


if __name__ == "__main__":
    for g in GLYPHS:
        process(g)
    print("done")
