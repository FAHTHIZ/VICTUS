#!/usr/bin/env python3
"""Generate the README artwork in ``assets/readme/`` and the hero in ``assets/``.

Every figure that shows a number reads it from the repository, so the README cannot drift from the code:
test cases are counted in ``tests/``, engine modules in ``engine/``, decision records in
``docs/TECHNOLOGY_DECISIONS.md`` and guides in ``docs/``. Run after any of those change:

    python3 scripts/readme_assets.py            # writes assets/victus-hero.svg and assets/readme/*.svg
    python3 scripts/readme_assets.py --png      # also renders assets/social-preview.png (needs cairosvg
                                                # or rsvg-convert)
    python3 scripts/readme_assets.py --check    # exits 1 if the committed files are out of date

The SVGs are self-contained, use no external fonts, images or scripts, and only animate when the viewer has
not asked for reduced motion. Their static frame is the finished state, so renderers without CSS animation
(and people who prefer reduced motion) see the complete picture.
"""

from __future__ import annotations

import math
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from html import escape
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets"
OUT = ASSETS / "readme"

FONT = "Inter, -apple-system, 'Segoe UI', Helvetica, Arial, sans-serif"
MONO = "'JetBrains Mono', 'SFMono-Regular', Menlo, Consolas, monospace"
CYAN, SKY, VIOLET, INDIGO, AMBER, ROSE = "#22D3EE", "#7DD3FC", "#A855F7", "#818CF8", "#F5B84A", "#FB7185"
REDUCED = "@media (prefers-reduced-motion: no-preference) {"


@dataclass(frozen=True)
class Theme:
    name: str
    bg: str
    card: str
    border: str
    text: str
    muted: str
    faint: str
    cyan: str
    sky: str
    violet: str
    indigo: str
    amber: str


DARK = Theme("dark", "#05070D", "#0D1424", "#1E2A44", "#E6F4FF", "#93A3C0", "#1B2640", CYAN, SKY, VIOLET, INDIGO, AMBER)
LIGHT = Theme(
    "light", "#FFFFFF", "#F4F7FC", "#D6DEEC", "#0B1222", "#55627A", "#E3E9F3",
    "#0891B2", "#0369A1", "#7E22CE", "#4F46E5", "#B7791F",
)

_written: dict[Path, str] = {}


# --------------------------------------------------------------------------------------------- numbers


def count_tests() -> int:
    pattern = re.compile(r"^\s*TEST_CASE(?:_TEMPLATE)?\(", re.M)
    total = 0
    for p in (ROOT / "tests").rglob("*.cpp"):
        total += len(pattern.findall(p.read_text(errors="replace")))
    return total


def count_modules() -> int:
    return sum(1 for p in (ROOT / "engine").iterdir() if p.is_dir() and (p / "CMakeLists.txt").exists())


def count_adrs() -> int:
    text = (ROOT / "docs" / "TECHNOLOGY_DECISIONS.md").read_text()
    nums = [int(n) for n in re.findall(r"^#{2,4} ADR-(\d{4})", text, re.M)]
    return max(nums) if nums else 0


def count_docs() -> int:
    return sum(1 for _ in (ROOT / "docs").glob("*.md"))


def count_samples() -> int:
    return sum(1 for p in (ROOT / "samples").iterdir() if p.is_dir() and p.name != "reference_scenes")


def text_width(s: str, size: float, bold: bool = True) -> float:
    """Approximate rendered width of ``s`` in a sans-serif face. Good enough to size pills and cells."""
    wide = sum(1 for c in s if c in "MWmw@%")
    narrow = sum(1 for c in s if c in "il.,:;|!'· +")
    upper = sum(1 for c in s if c.isupper() or c.isdigit())
    rest = len(s) - wide - narrow - upper
    per = 0.6 if bold else 0.55
    return size * (wide * 0.9 + narrow * 0.3 + upper * (per + 0.06) + rest * per)


def write(path: Path, svg: str) -> None:
    _written[path] = svg.strip() + "\n"


# ---------------------------------------------------------------------------------------------- badges

ICONS = {
    "tests": "M9 3h6M10 3v6l-5.4 9.4A2 2 0 0 0 6.3 21h11.4a2 2 0 0 0 1.7-2.6L14 9V3M7.5 15h9",
    "cpp": "M16 8a6 6 0 1 0 0 8M14 12h4M16 10v4M19 12h3M20.5 10.5v3",
    "license": "M12 3v18M5 7h14M5 7l-3 7a4 4 0 0 0 6 0zM19 7l-3 7a4 4 0 0 0 6 0zM8 21h8",
    "gpu": "M4 7h16v10H4zM8 7V4M12 7V4M16 7V4M8 20v-3M12 20v-3M16 20v-3M8 10h8v4H8z",
    "platforms": "M3 5h18v11H3zM8 20h8M12 16v4",
    "modules": "M12 3l8 4.5v9L12 21l-8-4.5v-9zM12 12l8-4.5M12 12v9M12 12L4 7.5",
    "adr": "M6 3h9l4 4v14H6zM14 3v5h5M9 13h7M9 17h5",
    "samples": "M6 11h4M8 9v4M15 12h.01M18 10h.01M17 4H7a5 5 0 0 0-5 5v4a5 5 0 0 0 9 3h2a5 5 0 0 0 9-3V9a5 5 0 0 0-5-5z",
    "nocode": "M4 4h7v7H4zM13 4h7v7h-7zM4 13h7v7H4zM16.5 13v7M13 16.5h7",
    "script": "M8 7l-5 5 5 5M16 7l5 5-5 5M14 4l-4 16",
    "net": "M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18zM3 12h18M12 3a14 14 0 0 1 0 18M12 3a14 14 0 0 0 0 18",
    "book": "M4 5a2 2 0 0 1 2-2h13v16H6a2 2 0 0 0-2 2zM4 21V5M19 19v2H6",
    "pr": "M6 3v12M6 15a3 3 0 1 0 0 6 3 3 0 0 0 0-6zM18 9a3 3 0 1 0 0-6 3 3 0 0 0 0 6zM18 9v3a3 3 0 0 1-3 3H9",
    "shield": "M12 3l8 3v6c0 5-3.5 8-8 9-4.5-1-8-4-8-9V6zM9 12l2 2 4-4",
}


def badge(slug: str, label: str, value: str, icon: str, c1: str, c2: str) -> None:
    lw = text_width(label, 10.5)
    vw = text_width(value, 12)
    val_w = vw + 24
    w = math.ceil(38 + lw + 10 + val_w + 3)
    vx = w - 3 - val_w
    uid = slug
    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="30" viewBox="0 0 {w} 30" role="img" aria-label="{escape(label.lower())}: {escape(value)}">
<title>{escape(label.lower())}: {escape(value)}</title>
<defs>
<linearGradient id="{uid}-bg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#111A2E"/><stop offset="1" stop-color="#05070D"/></linearGradient>
<linearGradient id="{uid}-val" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="{c1}"/><stop offset="1" stop-color="{c2}"/></linearGradient>
<linearGradient id="{uid}-edge" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="{c1}" stop-opacity=".6"/><stop offset=".5" stop-color="{c1}" stop-opacity=".15"/><stop offset="1" stop-color="{c2}" stop-opacity=".55"/></linearGradient>
<linearGradient id="{uid}-shine" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#fff" stop-opacity="0"/><stop offset=".5" stop-color="#fff" stop-opacity=".22"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></linearGradient>
<clipPath id="{uid}-clip"><rect width="{w}" height="30" rx="15"/></clipPath>
</defs>
<style>{REDUCED}
.sw{{animation:sw 9s 2s cubic-bezier(.4,0,.2,1) infinite}}
@keyframes sw{{0%{{transform:translateX(-80px)}}20%,100%{{transform:translateX({w + 80}px)}}}}
}}</style>
<g clip-path="url(#{uid}-clip)">
<rect width="{w}" height="30" fill="url(#{uid}-bg)"/>
<rect x="{vx:.1f}" y="3" width="{val_w:.1f}" height="24" rx="12" fill="url(#{uid}-val)"/>
<rect class="sw" x="-60" y="0" width="46" height="30" fill="url(#{uid}-shine)" transform="translate(-80 0)"/>
</g>
<rect x=".5" y=".5" width="{w - 1}" height="29" rx="14.5" fill="none" stroke="url(#{uid}-edge)"/>
<g transform="translate(12 6) scale(.75)" fill="none" stroke="{c1}" stroke-width="2.1" stroke-linecap="round" stroke-linejoin="round"><path d="{ICONS[icon]}"/></g>
<text x="38" y="18.8" font-family="{FONT}" font-size="10.5" font-weight="700" fill="#A9B7D0" letter-spacing=".04em">{escape(label)}</text>
<text x="{vx + val_w / 2:.1f}" y="19.4" text-anchor="middle" font-family="{FONT}" font-size="12" font-weight="700" fill="#05070D">{escape(value)}</text>
</svg>"""
    write(OUT / f"badge-{slug}.svg", svg)


# ------------------------------------------------------------------------------------------------ hero


def hero() -> None:
    """The animated banner: brand and promise on the left, a Studio window on the right.

    The window shows the three ways to make a game (Behaviours, Luau, C++) feeding a split 2D/3D viewport,
    with a play cursor and a render-graph strip underneath.
    """
    # 2D side of the viewport: platforms, coins, a player and a slime.
    coins = "".join(
        f'<circle class="coin" style="animation-delay:{i * 0.25:.2f}s" cx="{x}" cy="{y}" r="6" fill="{AMBER}"/>'
        for i, (x, y) in enumerate([(730, 316), (760, 304), (790, 316), (900, 290)])
    )
    # 3D side: a lit isometric cube and a sphere with a highlight, over a perspective grid.
    grid3d = "".join(
        f'<path d="M{1010 + i * 22} 412L{960 + i * 44} 466" stroke="{SKY}" stroke-opacity=".14"/>' for i in range(9)
    ) + "".join(f'<path d="M{1000 - i * 10} {412 + i * 12}H{1220 + i * 10}" stroke="{SKY}" stroke-opacity=".14"/>' for i in range(5))
    nodes = [("Depth", CYAN), ("Shadows", SKY), ("Lighting", INDIGO), ("SSR", VIOLET), ("TAA", ROSE), ("Tonemap", AMBER)]
    strip = ""
    for i, (name, c) in enumerate(nodes):
        x = 652 + i * 98
        strip += (
            f'<g class="node" style="animation-delay:{i * 0.35:.2f}s"><rect x="{x}" y="508" width="86" height="30" rx="9" fill="#0D1424" stroke="{c}" stroke-opacity=".7"/>'
            f'<circle cx="{x + 14}" cy="523" r="4" fill="{c}"/><text x="{x + 24}" y="527.5" font-family="{FONT}" font-size="11.5" font-weight="700" fill="#C9D6EA">{name}</text></g>'
        )
        if i < len(nodes) - 1:
            strip += f'<path d="M{x + 86} 523h12" stroke="#33415E" stroke-width="2"/>'
    paths = [("Behaviours", "no code", CYAN), ("Luau + graphs", "scripting", INDIGO), ("C++20", "native", VIOLET)]
    sources = ""
    for i, (title, sub, c) in enumerate(paths):
        x = 662 + i * 200
        sources += (
            f'<g><rect x="{x}" y="142" width="190" height="34" rx="10" fill="#0D1424" stroke="{c}" stroke-opacity=".75"/>'
            f'<circle cx="{x + 16}" cy="159" r="4.5" fill="{c}"/>'
            f'<text x="{x + 28}" y="163.5" font-family="{FONT}" font-size="13" font-weight="800" fill="#E6F4FF">{title}'
            f'<tspan font-size="11" font-weight="600" fill="{c}" dx="8">{sub}</tspan></text></g>'
            f'<path class="flow" d="M{x + 95} 176V196" fill="none" stroke="{c}" stroke-width="2" stroke-opacity=".8"/>'
        )
    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" width="1320" height="620" viewBox="0 0 1320 620" role="img" aria-labelledby="title desc">
<title id="title">VICTUS: the open engine for complete 2D and 3D games</title>
<desc id="desc">An animated illustration of VICTUS Studio. Behaviours, Luau and visual graphs, and C++ feed one viewport that shows a 2D platformer beside a lit 3D scene, with the render graph running underneath.</desc>
<defs>
<linearGradient id="bg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#05070D"/><stop offset=".55" stop-color="#0B1222"/><stop offset="1" stop-color="#140B24"/></linearGradient>
<linearGradient id="word" x1="0" x2="1"><stop offset="0" stop-color="#E0F7FF"/><stop offset=".5" stop-color="{SKY}"/><stop offset="1" stop-color="#C4B5FD"/></linearGradient>
<linearGradient id="accent" x1="0" x2="1"><stop offset="0" stop-color="{CYAN}"/><stop offset=".55" stop-color="{INDIGO}"/><stop offset="1" stop-color="{VIOLET}"/></linearGradient>
<linearGradient id="sky2d" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#1D2B57"/><stop offset="1" stop-color="#0F1730"/></linearGradient>
<linearGradient id="sky3d" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#2A1B4D"/><stop offset="1" stop-color="#0E1024"/></linearGradient>
<linearGradient id="cubeTop" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#A5F3FC"/><stop offset="1" stop-color="{CYAN}"/></linearGradient>
<linearGradient id="cubeL" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#0E7490"/><stop offset="1" stop-color="#083344"/></linearGradient>
<linearGradient id="cubeR" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#155E75"/><stop offset="1" stop-color="#0B3B4A"/></linearGradient>
<radialGradient id="ball" cx=".35" cy=".3" r=".75"><stop offset="0" stop-color="#F5D0FE"/><stop offset=".35" stop-color="{VIOLET}"/><stop offset="1" stop-color="#3B0764"/></radialGradient>
<radialGradient id="glowA" cx=".2" cy=".15" r=".6"><stop offset="0" stop-color="{CYAN}" stop-opacity=".28"/><stop offset="1" stop-color="{CYAN}" stop-opacity="0"/></radialGradient>
<radialGradient id="glowB" cx=".85" cy=".95" r=".6"><stop offset="0" stop-color="{VIOLET}" stop-opacity=".3"/><stop offset="1" stop-color="{VIOLET}" stop-opacity="0"/></radialGradient>
<pattern id="grid" width="34" height="34" patternUnits="userSpaceOnUse"><path d="M34 0H0V34" fill="none" stroke="#7DD3FC" stroke-opacity=".06"/></pattern>
<clipPath id="vp"><rect x="662" y="196" width="590" height="270" rx="10"/></clipPath>
</defs>
<style>{REDUCED}
.flow{{stroke-dasharray:6 8;animation:flow 1.6s linear infinite}}@keyframes flow{{to{{stroke-dashoffset:-28}}}}
.hero{{animation:run 4s ease-in-out infinite}}@keyframes run{{0%,100%{{transform:translateX(0)}}50%{{transform:translateX(150px)}}}}
.jump{{animation:jump 1s ease-in-out infinite}}@keyframes jump{{0%,100%{{transform:translateY(0)}}50%{{transform:translateY(-34px)}}}}
.coin{{transform-box:fill-box;transform-origin:center;animation:spin 1.4s ease-in-out infinite}}@keyframes spin{{0%,100%{{transform:scaleX(1)}}50%{{transform:scaleX(.2)}}}}
.slime{{transform-box:fill-box;transform-origin:bottom;animation:squish .9s ease-in-out infinite}}@keyframes squish{{0%,100%{{transform:scale(1,1)}}50%{{transform:scale(1.12,.85)}}}}
.orbit{{animation:bob 2.6s ease-in-out infinite}}@keyframes bob{{0%,100%{{transform:translateY(0)}}50%{{transform:translateY(-22px)}}}}
.node rect{{animation:pulse 2.1s ease-in-out infinite;animation-delay:inherit}}@keyframes pulse{{0%,100%{{stroke-opacity:.45}}50%{{stroke-opacity:1}}}}
.scan{{animation:scan 5s linear infinite}}@keyframes scan{{from{{transform:translateX(0)}}to{{transform:translateX(590px)}}}}
.l1{{animation:up 1s .1s cubic-bezier(.2,.7,.1,1) both}}.l2{{animation:up 1s .35s cubic-bezier(.2,.7,.1,1) both}}.l3{{animation:up 1s .6s cubic-bezier(.2,.7,.1,1) both}}
@keyframes up{{from{{opacity:0;transform:translateY(10px)}}to{{opacity:1;transform:none}}}}
}}</style>
<rect width="1320" height="620" rx="30" fill="url(#bg)"/>
<rect width="1320" height="620" rx="30" fill="url(#grid)"/>
<rect width="1320" height="620" rx="30" fill="url(#glowA)"/>
<rect width="1320" height="620" rx="30" fill="url(#glowB)"/>

<g class="l1" transform="translate(68 70)">
<path d="M0 0h16l14 34L44 0h16L38 52H22z" fill="url(#accent)"/>
<text x="76" y="40" font-family="{FONT}" font-size="34" font-weight="900" fill="url(#word)" letter-spacing=".12em">VICTUS</text>
</g>
<g class="l2" font-family="{FONT}" font-weight="800" letter-spacing="-.02em">
<text x="68" y="214" font-size="46" fill="#F1F8FF">Make the whole game.</text>
<text x="68" y="268" font-size="46" fill="url(#accent)">2D or 3D. No code</text>
<text x="68" y="322" font-size="46" fill="url(#accent)">required.</text>
</g>
<g class="l3" font-family="{FONT}">
<text x="68" y="374" font-size="18" font-weight="500" fill="#9FB0CC">An open C++20 engine with a full editor: PBR rendering,</text>
<text x="68" y="400" font-size="18" font-weight="500" fill="#9FB0CC">Jolt and Box2D physics, AI, animation, audio, multiplayer</text>
<text x="68" y="426" font-size="18" font-weight="500" fill="#9FB0CC">and one-command export.</text>
<g font-size="13" font-weight="700">
<rect x="68" y="462" width="126" height="34" rx="17" fill="#0D1424" stroke="{CYAN}" stroke-opacity=".6"/><text x="131" y="484" text-anchor="middle" fill="{CYAN}">Apache-2.0</text>
<rect x="204" y="462" width="194" height="34" rx="17" fill="#0D1424" stroke="{INDIGO}" stroke-opacity=".6"/><text x="301" y="484" text-anchor="middle" fill="#C7D2FE">Vulkan · Metal · D3D12</text>
<rect x="408" y="462" width="70" height="34" rx="17" fill="#0D1424" stroke="{VIOLET}" stroke-opacity=".6"/><text x="443" y="484" text-anchor="middle" fill="#E9D5FF">GLES</text>
</g>
<text x="68" y="540" font-family="{MONO}" font-size="14" fill="#6B7A99"><tspan fill="{CYAN}">$</tspan>&#160;victus new MyGame --template nocode</text>
</g>

<g>
<rect x="640" y="100" width="634" height="460" rx="18" fill="#080C17" stroke="#1E2A44"/>
<circle cx="664" cy="122" r="5" fill="#FB7185"/><circle cx="682" cy="122" r="5" fill="#F5B84A"/><circle cx="700" cy="122" r="5" fill="#34D399"/>
<text x="957" y="127" text-anchor="middle" font-family="{FONT}" font-size="12.5" font-weight="700" fill="#7B8BA8">VICTUS Studio · Play</text>
<g transform="translate(1214 112)"><rect width="40" height="20" rx="6" fill="#0E3B2E"/><path d="M16 5l10 5-10 5z" fill="#34D399"/></g>
<g clip-path="url(#vp)">
<rect x="662" y="196" width="295" height="270" fill="url(#sky2d)"/>
<rect x="957" y="196" width="295" height="270" fill="url(#sky3d)"/>
<circle cx="910" cy="236" r="20" fill="#FDE68A" fill-opacity=".85"/>
<rect x="662" y="412" width="295" height="60" fill="#1F3A2E"/><rect x="662" y="412" width="295" height="8" fill="#34D399" fill-opacity=".8"/>
<rect x="706" y="332" width="110" height="14" rx="4" fill="#3F5E8C"/><rect x="866" y="306" width="70" height="14" rx="4" fill="#3F5E8C"/>
{coins}
<g class="hero"><g class="jump"><rect x="684" y="378" width="24" height="34" rx="6" fill="{CYAN}"/><rect x="696" y="386" width="8" height="6" rx="2" fill="#05070D"/></g></g>
<path class="slime" d="M884 412c0-16 10-24 22-24s22 8 22 24z" fill="#4ADE80"/><circle cx="899" cy="402" r="2.5" fill="#05070D"/><circle cx="913" cy="402" r="2.5" fill="#05070D"/>
{grid3d}
<g transform="translate(1040 286)"><path d="M0 40L50 15l50 25-50 25z" fill="url(#cubeTop)"/><path d="M0 40v60l50 25V65z" fill="url(#cubeL)"/><path d="M100 40v60l-50 25V65z" fill="url(#cubeR)"/></g>
<ellipse cx="1090" cy="424" rx="62" ry="12" fill="#000" fill-opacity=".35"/>
<g class="orbit"><circle cx="1196" cy="360" r="28" fill="url(#ball)"/></g>
<ellipse cx="1196" cy="426" rx="26" ry="6" fill="#000" fill-opacity=".3"/>
<rect class="scan" x="662" y="196" width="2" height="270" fill="{CYAN}" fill-opacity=".35"/>
</g>
<path d="M957 196v270" stroke="#05070D" stroke-width="2"/>
<rect x="674" y="206" width="34" height="18" rx="5" fill="#05070D" fill-opacity=".6"/><text x="691" y="219" text-anchor="middle" font-family="{FONT}" font-size="10.5" font-weight="800" fill="{CYAN}">2D</text>
<rect x="969" y="206" width="34" height="18" rx="5" fill="#05070D" fill-opacity=".6"/><text x="986" y="219" text-anchor="middle" font-family="{FONT}" font-size="10.5" font-weight="800" fill="#E9D5FF">3D</text>
<text x="662" y="496" font-family="{FONT}" font-size="11" font-weight="700" fill="#5E6E8C" letter-spacing=".08em">RENDER GRAPH</text>
{strip}
</g>
{sources}
</svg>"""
    write(ASSETS / "victus-hero.svg", svg)


# --------------------------------------------------------------------------------------------- tagline


def tagline(t: Theme) -> None:
    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" width="1280" height="132" viewBox="0 0 1280 132" role="img" aria-labelledby="t d">
<title id="t">Make a whole game, 2D or 3D, without writing code.</title>
<desc id="d">Drop down to Luau, visual graphs or C++ whenever you want. One engine, one editor, from the first level to the exported build.</desc>
<defs><linearGradient id="g" x1="0" x2="1"><stop offset="0" stop-color="{t.cyan}"/><stop offset=".55" stop-color="{t.indigo}"/><stop offset="1" stop-color="{t.violet}"/></linearGradient></defs>
<style>{REDUCED}
.l1{{animation:up 1s .1s cubic-bezier(.2,.7,.1,1) both}}.l2{{animation:up 1s .45s cubic-bezier(.2,.7,.1,1) both}}
@keyframes up{{from{{opacity:0;transform:translateY(10px)}}to{{opacity:1;transform:none}}}}
}}</style>
<text class="l1" x="640" y="58" text-anchor="middle" font-family="{FONT}" font-size="40" font-weight="800" fill="url(#g)" letter-spacing="-.01em">Make a whole game, 2D or 3D, without writing code.</text>
<text class="l2" x="640" y="102" text-anchor="middle" font-family="{FONT}" font-size="19" font-weight="500" fill="{t.muted}">Drop down to Luau, visual graphs or C++ whenever you want. One engine, one editor, first level to shipped build.</text>
</svg>"""
    write(OUT / f"tagline-{t.name}.svg", svg)


# ----------------------------------------------------------------------------------------------- stats


def stats(t: Theme, cells: list[tuple[str, str, str, str]]) -> None:
    n = len(cells)
    gap = 20
    cw = (1280 - gap * (n - 1)) / n
    parts = []
    for i, (value, label, sub, color) in enumerate(cells):
        x = i * (cw + gap)
        parts.append(
            f"""<g class="c{i}">
<rect x="{x + 1:.1f}" y="1" width="{cw - 2:.1f}" height="150" rx="18" fill="{t.card}" stroke="{t.border}"/>
<rect class="b{i}" x="{x + 24:.1f}" y="124" width="{cw - 48:.1f}" height="4" rx="2" fill="{color}" opacity=".85"/>
<rect x="{x + 24:.1f}" y="124" width="{cw - 48:.1f}" height="4" rx="2" fill="{color}" opacity=".15"/>
<text x="{x + 24:.1f}" y="66" font-family="{FONT}" font-size="44" font-weight="800" fill="{color}" letter-spacing="-.02em">{escape(value)}</text>
<text x="{x + 24:.1f}" y="92" font-family="{FONT}" font-size="15" font-weight="700" fill="{t.text}">{escape(label)}</text>
<text x="{x + 24:.1f}" y="112" font-family="{FONT}" font-size="12.5" font-weight="500" fill="{t.muted}">{escape(sub)}</text>
</g>"""
        )
    anim = "".join(
        f".c{i}{{animation:fade .9s {0.15 + i * 0.18:.2f}s cubic-bezier(.2,.7,.1,1) both}}"
        f".b{i}{{transform-box:fill-box;transform-origin:left;animation:grow 1.6s {0.4 + i * 0.18:.2f}s cubic-bezier(.2,.7,.1,1) both}}"
        for i in range(n)
    )
    desc = "; ".join(f"{v} {lab} ({s})" for v, lab, s, _ in cells)
    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" width="1280" height="152" viewBox="0 0 1280 152" role="img" aria-labelledby="t d">
<title id="t">VICTUS in numbers</title>
<desc id="d">{escape(desc)}</desc>
<style>{REDUCED}
{anim}
@keyframes fade{{from{{opacity:0;transform:translateY(8px)}}to{{opacity:1;transform:none}}}}
@keyframes grow{{from{{transform:scaleX(0)}}to{{transform:scaleX(1)}}}}
}}</style>
{"".join(parts)}
</svg>"""
    write(OUT / f"stats-{t.name}.svg", svg)


# ------------------------------------------------------------------------------------------ quickstart


def quickstart(t: Theme) -> None:
    steps = [
        ("01", "Build it", "cmake --preset dev && cmake --build --preset dev", "Pinned, hash-verified dependencies."),
        ("02", "Create a game", "victus new MyGame --template nocode", "A finished level made from Behaviours."),
        ("03", "Play and ship", "victus export MyGame --out MyGame-export", "Signed .vpak packs and a standalone player."),
    ]
    gap = 24
    cw = (1280 - gap * 2) / 3
    parts = []
    colors = [t.cyan, t.indigo, t.violet]
    for i, (num, title, cmd, sub) in enumerate(steps):
        x = i * (cw + gap)
        c = colors[i]
        arrow = ""
        if i < 2:
            ax = x + cw + 2
            arrow = (
                f'<path d="M{ax:.1f} 90h{gap - 4}" stroke="{t.border}" stroke-width="2"/>'
                f'<path d="M{ax + gap - 10:.1f} 85l6 5-6 5" fill="none" stroke="{t.border}" stroke-width="2"/>'
            )
        size = 12.5 if len(cmd) < 42 else 11.2
        parts.append(
            f"""<g class="s{i}">
<rect x="{x + 1:.1f}" y="1" width="{cw - 2:.1f}" height="166" rx="18" fill="{t.card}" stroke="{t.border}"/>
<text x="{x + 26:.1f}" y="50" font-family="{FONT}" font-size="30" font-weight="800" fill="{c}">{num}</text>
<text x="{x + 76:.1f}" y="47" font-family="{FONT}" font-size="18" font-weight="800" fill="{t.text}">{escape(title)}</text>
<rect x="{x + 22:.1f}" y="70" width="{cw - 44:.1f}" height="40" rx="10" fill="{t.bg}" stroke="{t.border}"/>
<text x="{x + 34:.1f}" y="95" font-family="{MONO}" font-size="{size}" fill="{t.text}"><tspan fill="{c}">$</tspan>&#160;{escape(cmd)}</text>
<text x="{x + 26:.1f}" y="142" font-family="{FONT}" font-size="13.5" fill="{t.muted}">{escape(sub)}</text>
</g>{arrow}"""
        )
    anim = "".join(f".s{i}{{animation:fade .9s {0.15 + i * 0.3:.2f}s cubic-bezier(.2,.7,.1,1) both}}" for i in range(3))
    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" width="1280" height="168" viewBox="0 0 1280 168" role="img" aria-labelledby="t d">
<title id="t">From clone to shipped game in three steps</title>
<desc id="d">01 build with the dev CMake preset; 02 create a game with victus new and the nocode template; 03 export it with victus export.</desc>
<style>{REDUCED}
{anim}
@keyframes fade{{from{{opacity:0;transform:translateY(8px)}}to{{opacity:1;transform:none}}}}
}}</style>
{"".join(parts)}
</svg>"""
    write(OUT / f"quickstart-{t.name}.svg", svg)


# ------------------------------------------------------------------------------------- icons, dividers

FEATURE_ICONS = {
    "nocode": (ICONS["nocode"], CYAN),
    "render": ("M12 3l9 5v8l-9 5-9-5V8zM12 13l9-5M12 13v8M12 13L3 8", SKY),
    "physics": ("M12 21a3 3 0 1 0 0-6 3 3 0 0 0 0 6zM5 9a3 3 0 1 0 0-6 3 3 0 0 0 0 6zM19 9a3 3 0 1 0 0-6 3 3 0 0 0 0 6zM7 7.5l3.5 8M17 7.5l-3.5 8M8 6h8", INDIGO),
    "studio": ("M3 4h18v14H3zM3 8h18M8 8v10M7 21h10", VIOLET),
    "net": (ICONS["net"], AMBER),
    "ship": ("M12 3v12M7 10l5 5 5-5M4 17v3h16v-3", CYAN),
}


def feature_icon(name: str, t: Theme) -> None:
    path, accent = FEATURE_ICONS[name]
    color = {CYAN: t.cyan, SKY: t.sky, INDIGO: t.indigo, VIOLET: t.violet, AMBER: t.amber}[accent]
    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48" viewBox="0 0 48 48" aria-hidden="true">
<rect x=".5" y=".5" width="47" height="47" rx="14" fill="{color}" fill-opacity=".12" stroke="{color}" stroke-opacity=".45"/>
<g transform="translate(12 12)" fill="none" stroke="{color}" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="{path}"/></g>
</svg>"""
    write(OUT / f"icon-{name}-{t.name}.svg", svg)


def divider(t: Theme) -> None:
    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" width="1280" height="24" viewBox="0 0 1280 24" aria-hidden="true">
<defs><linearGradient id="s" x1="0" x2="1"><stop offset="0" stop-color="{t.cyan}" stop-opacity="0"/><stop offset=".5" stop-color="{t.cyan}"/><stop offset="1" stop-color="{t.violet}" stop-opacity="0"/></linearGradient>
<linearGradient id="v" x1="0" x2="1"><stop offset="0" stop-color="{t.cyan}"/><stop offset="1" stop-color="{t.violet}"/></linearGradient></defs>
<style>{REDUCED}
.sweep{{animation:sweep 6s ease-in-out infinite}}@keyframes sweep{{from{{transform:translateX(-320px)}}to{{transform:translateX(1280px)}}}}
}}</style>
<path d="M0 12H1280" stroke="{t.faint}" stroke-width="1.5"/>
<rect class="sweep" x="0" y="11" width="320" height="2" fill="url(#s)" transform="translate(-320 0)"/>
<g transform="translate(628 0)"><rect width="24" height="24" rx="8" fill="{t.bg}" stroke="{t.cyan}" stroke-opacity=".7"/><path d="M6 7h3l3 7 3-7h3l-4.5 10h-3z" fill="url(#v)"/></g>
</svg>"""
    write(OUT / f"divider-{t.name}.svg", svg)


def footer() -> None:
    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" width="1280" height="120" viewBox="0 0 1280 120" role="img" aria-label="VICTUS. Make the whole game. 2D or 3D. Behaviours, Luau or C++.">
<defs>
<linearGradient id="bg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#05070D"/><stop offset="1" stop-color="#140B24"/></linearGradient>
<linearGradient id="g" x1="0" x2="1"><stop offset="0" stop-color="{CYAN}"/><stop offset=".55" stop-color="{INDIGO}"/><stop offset="1" stop-color="{VIOLET}"/></linearGradient>
</defs>
<style>{REDUCED}
.p{{stroke-dasharray:8 10;animation:flow 2.4s linear infinite}}@keyframes flow{{to{{stroke-dashoffset:-36}}}}
}}</style>
<rect width="1280" height="120" rx="22" fill="url(#bg)"/>
<path class="p" d="M60 96C220 96 330 40 560 40" fill="none" stroke="url(#g)" stroke-opacity=".5" stroke-width="2"/>
<path class="p" d="M1220 96C1060 96 950 40 760 40" fill="none" stroke="url(#g)" stroke-opacity=".5" stroke-width="2"/>
<g transform="translate(578 22)"><rect width="36" height="36" rx="11" fill="#0D1424" stroke="{CYAN}" stroke-opacity=".75"/><path d="M8 9h4.5l5.5 12.5L23.5 9H28l-8 19h-4z" fill="url(#g)"/></g>
<text x="626" y="47" font-family="{FONT}" font-size="20" font-weight="900" fill="#E6F4FF" letter-spacing=".12em">VICTUS</text>
<text x="640" y="88" text-anchor="middle" font-family="{FONT}" font-size="14" font-weight="600" fill="#93A3C0">Make the whole game · 2D or 3D · Behaviours, Luau or C++ · Back to top</text>
</svg>"""
    write(OUT / "footer.svg", svg)


def social_svg(tests: int, modules: int, adrs: int) -> str:
    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="1280" height="640" viewBox="0 0 1280 640">
<defs>
<linearGradient id="bg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#05070D"/><stop offset=".6" stop-color="#0B1222"/><stop offset="1" stop-color="#140B24"/></linearGradient>
<linearGradient id="g" x1="0" x2="1"><stop offset="0" stop-color="{CYAN}"/><stop offset=".55" stop-color="{INDIGO}"/><stop offset="1" stop-color="{VIOLET}"/></linearGradient>
<radialGradient id="halo"><stop offset="0" stop-color="{VIOLET}" stop-opacity=".25"/><stop offset="1" stop-color="{VIOLET}" stop-opacity="0"/></radialGradient>
<linearGradient id="cubeTop" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#A5F3FC"/><stop offset="1" stop-color="{CYAN}"/></linearGradient>
<pattern id="grid" width="40" height="40" patternUnits="userSpaceOnUse"><path d="M40 0H0V40" fill="none" stroke="#7DD3FC" stroke-opacity=".07"/></pattern>
</defs>
<rect width="1280" height="640" fill="url(#bg)"/><rect width="1280" height="640" fill="url(#grid)"/>
<circle cx="1030" cy="320" r="330" fill="url(#halo)"/>
<g transform="translate(80 84)"><path d="M0 0h18l16 38L50 0h18L44 58H24z" fill="url(#g)"/></g>
<text x="170" y="132" font-family="{FONT}" font-size="46" font-weight="900" fill="#F5F8FF" letter-spacing=".12em">VICTUS</text>
<text x="80" y="250" font-family="{FONT}" font-size="58" font-weight="800" fill="url(#g)" letter-spacing="-.02em">Make the whole game.</text>
<text x="80" y="320" font-family="{FONT}" font-size="58" font-weight="800" fill="url(#g)" letter-spacing="-.02em">2D or 3D. No code required.</text>
<text x="80" y="380" font-family="{FONT}" font-size="24" font-weight="500" fill="#A9B7D0">Open C++20 engine and editor: PBR rendering, Jolt and Box2D,</text>
<text x="80" y="414" font-family="{FONT}" font-size="24" font-weight="500" fill="#A9B7D0">Luau and visual scripting, multiplayer and one-command export.</text>
<g font-family="{FONT}" font-weight="800">
<rect x="80" y="468" width="240" height="96" rx="18" fill="#0D1424" stroke="#1E2A44"/>
<text x="104" y="516" font-size="38" fill="{CYAN}">{tests:,}</text><text x="104" y="546" font-size="15" font-weight="600" fill="#93A3C0">automated test cases</text>
<rect x="340" y="468" width="240" height="96" rx="18" fill="#0D1424" stroke="#1E2A44"/>
<text x="364" y="516" font-size="38" fill="{INDIGO}">{modules}</text><text x="364" y="546" font-size="15" font-weight="600" fill="#93A3C0">engine modules</text>
<rect x="600" y="468" width="240" height="96" rx="18" fill="#0D1424" stroke="#1E2A44"/>
<text x="624" y="516" font-size="38" fill="{VIOLET}">{adrs}</text><text x="624" y="546" font-size="15" font-weight="600" fill="#93A3C0">decision records</text>
</g>
<g transform="translate(960 220)"><path d="M0 60L80 20l80 40-80 40z" fill="url(#cubeTop)"/><path d="M0 60v100l80 40V100z" fill="#0E7490"/><path d="M160 60v100l-80 40V100z" fill="#155E75"/></g>
<circle cx="1180" cy="200" r="42" fill="{VIOLET}" fill-opacity=".85"/>
</svg>"""


# ------------------------------------------------------------------------------------------------ main


def main() -> int:
    tests, modules, adrs, docs, samples = count_tests(), count_modules(), count_adrs(), count_docs(), count_samples()

    for slug, label, value, icon, c1, c2 in (
        ("tests", "TESTS", f"{tests:,} cases", "tests", "#34D399", "#6EE7B7"),
        ("cpp", "C++", "20", "cpp", INDIGO, "#A5B4FC"),
        ("license", "LICENSE", "Apache 2.0", "license", CYAN, SKY),
        ("gpu", "GPU", "Vulkan · Metal · D3D12 · GLES", "gpu", VIOLET, "#C084FC"),
        ("platforms", "PLATFORMS", "Linux · Windows · macOS · Android", "platforms", SKY, CYAN),
        ("modules", "ENGINE", f"{modules} modules", "modules", CYAN, INDIGO),
        ("adr", "DECISIONS", f"{adrs} ADRs", "adr", INDIGO, VIOLET),
        ("samples", "SAMPLES", f"{samples} finished games", "samples", AMBER, "#FCD34D"),
        ("nocode", "NO-CODE", "Behaviours", "nocode", CYAN, "#67E8F9"),
        ("script", "SCRIPTING", "Luau · graphs · C++", "script", VIOLET, INDIGO),
        ("net", "MULTIPLAYER", "server-authoritative", "net", AMBER, ROSE),
        ("docs", "DOCS", f"{docs} guides", "book", SKY, INDIGO),
        ("security", "SECURITY", "policy", "shield", "#34D399", CYAN),
        ("prs", "PULL REQUESTS", "welcome", "pr", VIOLET, "#C084FC"),
    ):
        badge(slug, label, value, icon, c1, c2)

    hero()
    for t in (DARK, LIGHT):
        tagline(t)
        stats(
            t,
            [
                (f"{tests:,}", "automated test cases", "CTest on Linux, Windows, macOS", t.cyan),
                (str(modules), "engine modules", "rendering to networking", t.indigo),
                (str(adrs), "decision records", "every architectural choice", t.violet),
                ("6", "ready-made templates", "including two with no code", t.amber),
            ],
        )
        quickstart(t)
        divider(t)
        for name in FEATURE_ICONS:
            feature_icon(name, t)
    footer()
    social = social_svg(tests, modules, adrs)
    write(OUT / "social-preview.svg", social)

    if "--check" in sys.argv:
        stale = [p for p, s in _written.items() if not p.exists() or p.read_text() != s]
        for p in stale:
            print(f"out of date: {p.relative_to(ROOT)}")
        if stale:
            print("run: python3 scripts/readme_assets.py")
        return 1 if stale else 0

    for p, s in _written.items():
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(s)
    if "--png" in sys.argv:
        png = ASSETS / "social-preview.png"
        try:
            import cairosvg  # type: ignore[import-not-found]

            cairosvg.svg2png(bytestring=social.encode(), write_to=str(png))
        except ImportError:
            tool = shutil.which("rsvg-convert")
            if not tool:
                print("--png needs cairosvg or rsvg-convert", file=sys.stderr)
                return 1
            subprocess.run([tool, "-o", str(png), str(OUT / "social-preview.svg")], check=True)
    print(f"wrote {len(_written)} files (tests {tests}, modules {modules}, ADRs {adrs}, docs {docs})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
