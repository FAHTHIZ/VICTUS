# SPDX-License-Identifier: Apache-2.0
"""Per-texel PBR maps of the unhelmeted head parts (human.py, docs/CHARACTERS.md, ADR-0247).

Skin, eyes, mouth and the groom shells reuse the human-pilot functions in materials.py (measured
skin albedo/roughness ranges, iris stroma, pores, brows and lashes); this module adds the bob's
strand cards. Every function returns the contract build_characters.bake_part expects:
{"color", "rough", "metal", "height", "cavity"} plus "alpha" for the Hair atlas.
"""
from __future__ import annotations

import numpy as np

import human
import materials


def _smooth(t):
    t = np.clip(t, 0, 1)
    return t * t * (3 - 2 * t)


def card_maps(s, t, tile, layer, hs, seed):
    """Strand tiles: ~64 strands per tile, each wandering slightly across the card, tapering at a
    ragged blunt tip (the bob's cut), clumped in brightness; inner layers darker."""
    n = len(s)
    alpha = np.zeros(n)
    h = np.zeros(n)
    tone = np.ones(n)
    for k in range(human.TILES):
        m = tile == k
        if not m.any():
            continue
        rng = np.random.default_rng(seed * 31 + k)
        ns = 120
        x0 = rng.random(ns)
        wid = rng.uniform(0.006, 0.016, ns)
        tend = 1.0 - rng.random(ns) ** 2 * 0.20
        freq = rng.uniform(0.4, 1.6, ns)
        ph = rng.random(ns) * 2 * np.pi
        amp = rng.uniform(0.003, 0.014, ns)
        br = rng.uniform(0.45, 1.6, ns)
        ss, tt = s[m], t[m]
        a = np.zeros(len(ss))
        hh = np.zeros(len(ss))
        tn = np.ones(len(ss))
        for i in range(ns):
            xc = x0[i] + amp[i] * np.sin(2 * np.pi * freq[i] * tt + ph[i])
            w = wid[i] * (1.0 - 0.65 * _smooth((tt - (tend[i] - 0.10)) / 0.10))
            prof = np.clip(1.0 - np.abs(ss - xc) / np.maximum(w, 1e-4), 0, 1) * (tt < tend[i])
            up = prof > a
            tn = np.where(up, br[i], tn)
            a = np.maximum(a, prof)
            hh = np.maximum(hh, np.sqrt(prof))
        edge = _smooth(ss / 0.10) * _smooth((1 - ss) / 0.10)
        alpha[m] = np.clip(a * 2.4, 0, 1) * (0.35 + 0.65 * edge)
        h[m] = hh * 0.00004
        tone[m] = tn
    inner = (tile >= human.TILES // 2).astype(float) * 0.8
    del layer
    col = np.array(hs.hair)[None, :] * (tone * (0.75 + 0.35 * t) * (1 - 0.35 * inner))[:, None]
    rough = hs.hair_rough + 0.12 * (1 - np.clip(h / 0.00004, 0, 1)) + 0.05 * inner
    cavity = (1 - 0.35 * inner) * (0.7 + 0.3 * _smooth(t / 0.25))
    return {"color": col, "rough": rough, "metal": np.zeros(n), "height": h, "alpha": alpha, "cavity": cavity}


def _empty(n):
    return {"color": np.zeros((n, 3)), "rough": np.full(n, 0.5), "metal": np.zeros(n), "height": np.zeros(n),
            "cavity": np.ones(n), "alpha": np.zeros(n)}


def _put(out, m, sub):
    for k, v in sub.items():
        if k in out:
            out[k][m] = v


def maps(name, tx, P, N, L, hs, attrs, face_attrs, fa, seed):
    n = len(P)
    if name == "Skin":
        return materials.skin_maps(tx, P, N, L, hs, seed)
    if name == "Eye":
        kind = face_attrs["kind"][fa]
        out = _empty(n)
        del out["alpha"]
        for k, fn in ((0, lambda m: materials.eye_maps(tx, P[m], N[m], {a: v[m] for a, v in attrs.items()}, hs, seed)),
                      (1, lambda m: materials.mouth_maps(tx, P[m], N[m], {a: v[m] for a, v in attrs.items()}, seed))):
            m = kind == k
            if m.any():
                _put(out, m, fn(m))
        return out
    if name == "Hair":
        kind = face_attrs["kind"][fa]
        out = _empty(n)
        g = kind < 3
        if g.any():
            sub = materials.groom_maps(tx, P[g], N[g], {a: v[g] for a, v in attrs.items()}, hs, seed)
            # Scalp hair shares the cards' sheen.
            shell = np.rint(attrs["kind"][g, 0]) == 0
            sub["rough"] = np.where(shell, np.clip(sub["rough"] - (0.72 - hs.hair_rough), 0.05, 1), sub["rough"])
            _put(out, g, sub)
        c = kind == 3
        if c.any():
            tile = face_attrs["tile"][fa][c]
            _put(out, c, card_maps(attrs["s"][c, 0], attrs["t"][c, 0], tile, attrs["layer"][c, 0], hs, seed))
        return out
    raise ValueError(name)
