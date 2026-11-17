#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Regenerates assets/textures/dungeon_tiles.png (16 px pixel-art tiles, 4 columns x 7 rows).

Tiles 0-15: stone wall autotile, indexed by which side neighbours are also wall
(north 1, east 2, south 4, west 8) -- a darker rim is drawn on every open side.
Tiles 16-18: floor variants. 19: closed exit. 20: open exit. 21-23: floor decorations.
Tiles 24-27 (transparent background): hero, gem, slime, heart -- used as sprite frames.
Deterministic: the same script always writes the same pixels. Needs Pillow.
"""
import random
from pathlib import Path

from PIL import Image

T = 16
COLS, ROWS = 4, 7
img = Image.new("RGBA", (COLS * T, ROWS * T), (0, 0, 0, 0))
px = img.load()
rng = random.Random(1234)


def tile_origin(i):
    return (i % COLS) * T, (i // COLS) * T


def put(i, x, y, c):
    ox, oy = tile_origin(i)
    px[ox + x, oy + y] = c


def shade(c, k):
    return tuple(max(0, min(255, int(v * k))) for v in c[:3]) + (255,)


STONE = (92, 88, 110)
FLOOR = (58, 50, 46)

# Walls: brick pattern with a dark rim on open sides and a highlight on top.
for mask in range(16):
    for y in range(T):
        for x in range(T):
            row = y // 4
            mortar = (y % 4 == 3) or ((x + (row % 2) * 4) % 8 == 7)
            c = shade(STONE, 0.62 if mortar else 0.9 + 0.2 * rng.random())
            put(mask, x, y, c)
    # Image y grows downwards; north is the top row of the tile.
    open_n, open_e, open_s, open_w = not mask & 1, not mask & 2, not mask & 4, not mask & 8
    for k in range(T):
        for d in range(2):
            if open_n:
                put(mask, k, d, shade(STONE, 1.35 if d == 0 else 1.15))
            if open_s:
                put(mask, k, T - 1 - d, shade(STONE, 0.35))
            if open_w:
                put(mask, d, k, shade(STONE, 0.5))
            if open_e:
                put(mask, T - 1 - d, k, shade(STONE, 0.5))

# Floors.
for i, tint in zip((16, 17, 18), (1.0, 0.92, 1.06)):
    for y in range(T):
        for x in range(T):
            grout = x % 8 == 0 or y % 8 == 0
            put(i, x, y, shade(FLOOR, (0.7 if grout else tint) * (0.95 + 0.1 * rng.random())))

# Exits: closed (dark gate) and open (glowing portal) on a floor background.
for i, open_ in ((19, False), (20, True)):
    for y in range(T):
        for x in range(T):
            put(i, x, y, shade(FLOOR, 0.9))
            dx, dy = x - 7.5, y - 7.5
            r2 = dx * dx + dy * dy
            if r2 < 42:
                if open_:
                    g = 1.0 - r2 / 42
                    put(i, x, y, (int(80 + 150 * g), int(200 + 55 * g), int(255), 255))
                else:
                    put(i, x, y, (40, 34, 52, 255) if (x // 2) % 2 else (70, 60, 84, 255))

# Decorations: cracks, bones, moss.
for i, col in ((21, (30, 26, 24, 255)), (22, (210, 200, 180, 255)), (23, (70, 120, 60, 255))):
    for y in range(T):
        for x in range(T):
            put(i, x, y, shade(FLOOR, 0.98))
    for _ in range(14):
        put(i, rng.randrange(3, 13), rng.randrange(3, 13), col)



def blob(i, cells, palette):
    """Draws a 16x16 sprite from rows of characters ('.' = transparent)."""
    for y, row in enumerate(cells):
        for x, ch in enumerate(row):
            if ch != ".":
                put(i, x, y, palette[ch])


HERO = [
    "................",
    ".....kbbbbk.....",
    "....kbbbbbbk....",
    "....kSSSSSSk....",
    "....kSsSSsSk....",
    ".....kSSSSk.....",
    "....kBBBBBBk....",
    "...kBBbBBbBBk...",
    "...kBBBBBBBBk...",
    "...kSkBBBBkSk...",
    "....kkBBBBkk....",
    ".....kBBBBk.....",
    ".....kBk.kBk....",
    ".....kBk.kBk....",
    ".....kkk.kkk....",
    "................",
]
GEM = [
    "................",
    "................",
    "......kkkk......",
    ".....kWGGGk.....",
    "....kWGGGGGk....",
    "...kWGGGGGGGk...",
    "...kGGGGGGGgk...",
    "...kGGGGGGggk...",
    "....kGGGGggk....",
    ".....kGGggk.....",
    "......kggk......",
    ".......kk.......",
    "................",
    "................",
    "................",
    "................",
]
SLIME = [
    "................",
    "................",
    "................",
    "................",
    "................",
    "......kkkk......",
    "....kkRRRRkk....",
    "...kRRRRRRRRk...",
    "..kRRWkRRWkRRk..",
    "..kRRkkRRkkRRk..",
    "..kRRRRRRRRRRk..",
    ".kRRRRRRRRRRRRk.",
    ".kRRRRRRRRRRRrk.",
    ".krrRRRRRRRrrrk.",
    "..kkkkkkkkkkkk..",
    "................",
]
HEART = [
    "................",
    "................",
    "...kkk....kkk...",
    "..kHHHk..kHHHk..",
    ".kHWHHHkkHHHHHk.",
    ".kHWHHHHHHHHHHk.",
    ".kHHHHHHHHHHHHk.",
    "..kHHHHHHHHHHk..",
    "...kHHHHHHHHk...",
    "....kHHHHHHk....",
    ".....kHHHHk.....",
    "......kHHk......",
    ".......kk.......",
    "................",
    "................",
    "................",
]
pal = {
    "k": (20, 16, 28, 255), "S": (240, 200, 160, 255), "s": (60, 40, 40, 255),
    "B": (60, 120, 230, 255), "b": (240, 210, 80, 255),
    "W": (255, 255, 255, 255), "G": (70, 230, 140, 255), "g": (30, 150, 90, 255),
    "R": (220, 60, 70, 255), "r": (150, 30, 50, 255), "H": (240, 50, 80, 255),
}
blob(24, HERO, pal)
blob(25, GEM, pal)
blob(26, SLIME, pal)
blob(27, HEART, pal)

out = Path(__file__).resolve().parent.parent / "assets" / "textures" / "dungeon_tiles.png"
img.save(out)
print("wrote", out)
