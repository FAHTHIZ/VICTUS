#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Regenerates assets/textures/ruin_tiles.png (16 px pixel-art tiles, 4 columns x 4 rows).

0 grass-topped earth, 1 earth, 2 stone block, 3 one-way plank (solid tiles: 0-2; one-way: 3).
4 spikes, 5 checkpoint (lowered flag), 6 checkpoint (raised flag), 7 goal banner, 14 bush,
15 hanging vine (decor, no collision). Sprite frames (transparent background): 8-10 hero
idle/run/jump, 11 coin, 12-13 beetle walk frames.
Deterministic: the same script always writes the same pixels. Needs Pillow.
"""
import random
from pathlib import Path

from PIL import Image

T = 16
COLS, ROWS = 4, 4
img = Image.new("RGBA", (COLS * T, ROWS * T), (0, 0, 0, 0))
px = img.load()
rng = random.Random(4321)


def put(i, x, y, c):
    if 0 <= x < T and 0 <= y < T:
        px[(i % COLS) * T + x, (i // COLS) * T + y] = c


def shade(c, k):
    return tuple(max(0, min(255, int(v * k))) for v in c[:3]) + (255,)


def rect(i, x0, y0, x1, y1, c):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            put(i, x, y, c)


EARTH = (122, 82, 52)
GRASS = (88, 170, 64)
STONE = (128, 124, 140)
WOOD = (160, 112, 60)

# 0/1: earth with pebbles; 0 has a grass cap with a jagged edge.
for t in (0, 1):
    for y in range(T):
        for x in range(T):
            put(t, x, y, shade(EARTH, 0.85 + 0.25 * rng.random()))
    for _ in range(6):
        x, y = rng.randrange(1, T - 2), rng.randrange(5, T - 2)
        rect(t, x, y, x + 1, y, shade(EARTH, 0.55))
for x in range(T):
    depth = 4 + (1 if (x * 7) % 5 == 0 else 0)
    for y in range(depth):
        put(0, x, y, shade(GRASS, 1.25 if y == 0 else 0.95 + 0.15 * rng.random()))
# 2: stone block with bevel.
for y in range(T):
    for x in range(T):
        k = 0.9 + 0.15 * rng.random()
        if x == 0 or y == 0:
            k = 1.3
        if x == T - 1 or y == T - 1:
            k = 0.55
        put(2, x, y, shade(STONE, k))
rect(2, 3, 7, 12, 7, shade(STONE, 0.7))
# 3: plank (top 5 rows; the rest transparent).
for y in range(5):
    for x in range(T):
        put(3, x, y, shade(WOOD, 1.25 if y == 0 else (0.6 if y == 4 or x % 8 == 7 else 0.95)))
# 4: spikes standing on the tile's floor.
for s in range(3):
    for h in range(10):
        half = (10 - h) // 4
        for w in range(-half, half + 1):
            put(4, s * 5 + 3 + w, T - 1 - h, (225, 225, 238, 255) if w <= 0 else (140, 140, 160, 255))
# 5/6: checkpoint post with a lowered (grey) or raised (green) flag; 7: goal banner (gold).
for t, top, colour in ((5, 9, (140, 140, 150, 255)), (6, 1, (90, 220, 110, 255)), (7, 1, (250, 200, 60, 255))):
    rect(t, 3, 1, 4, T - 1, (90, 70, 50, 255))
    rect(t, 5, top, 12, top + 5, colour)
    rect(t, 5, top + 5, 12, top + 5, shade(colour, 0.6))
rect(7, 2, 0, 5, 1, (255, 235, 140, 255))
# 14: bush. 15: vine.
for y in range(6, T):
    for x in range(T):
        dx, dy = (x - 7.5) / 8, (y - 15) / 9
        if dx * dx + dy * dy < 1:
            put(14, x, y, shade(GRASS, 0.7 + 0.35 * rng.random()))
for y in range(T):
    x = 7 + (1 if (y // 3) % 2 else 0)
    put(15, x, y, shade(GRASS, 0.8))
    if y % 4 == 1:
        rect(15, x - 2, y, x - 1, y + 1, shade(GRASS, 1.1))

# Sprites. Hero: a small explorer (hat, face, tunic, legs posed per frame).
SKIN, HAT, TUNIC, BOOT = (240, 196, 150, 255), (150, 60, 40, 255), (60, 110, 200, 255), (60, 40, 30, 255)
for t, legs in ((8, "stand"), (9, "run"), (10, "jump")):
    rect(t, 4, 1, 11, 2, HAT)
    rect(t, 3, 3, 12, 3, HAT)
    rect(t, 5, 4, 10, 7, SKIN)
    put(t, 9, 5, (20, 20, 30, 255))
    rect(t, 4, 8, 11, 11, TUNIC)
    if legs == "stand":
        rect(t, 5, 12, 6, 15, BOOT)
        rect(t, 9, 12, 10, 15, BOOT)
    elif legs == "run":
        rect(t, 3, 12, 4, 14, BOOT)
        rect(t, 10, 12, 12, 13, BOOT)
        rect(t, 11, 14, 12, 15, BOOT)
    else:
        rect(t, 4, 12, 6, 13, BOOT)
        rect(t, 9, 12, 11, 13, BOOT)
    if legs == "jump":
        rect(t, 12, 6, 13, 8, SKIN)  # raised arm
# Coin.
for y in range(T):
    for x in range(T):
        dx, dy = (x - 7.5) / 5.5, (y - 7.5) / 6.5
        if dx * dx + dy * dy < 1:
            put(11, x, y, (250, 205, 50, 255) if dx < 0.35 else (200, 150, 30, 255))
rect(11, 7, 4, 8, 11, (255, 240, 150, 255))
# Beetle (two walk frames).
for t, phase in ((12, 0), (13, 1)):
    for y in range(4, 13):
        for x in range(T):
            dx, dy = (x - 7.5) / 7.5, (y - 12) / 8
            if dx * dx + dy * dy < 1:
                put(t, x, y, (150, 40, 160, 255) if y > 6 else (200, 90, 210, 255))
    rect(t, 7, 5, 8, 12, (90, 20, 100, 255))
    put(t, 12, 9, (255, 255, 255, 255))
    for k, x in enumerate((2, 6, 10, 13)):
        y = 13 + ((k + phase) % 2)
        rect(t, x, 13, x, y + 1, (40, 20, 40, 255))

out = Path(__file__).resolve().parent.parent / "assets" / "textures" / "ruin_tiles.png"
img.save(out)
print("wrote", out)
