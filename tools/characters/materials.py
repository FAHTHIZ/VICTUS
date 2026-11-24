# SPDX-License-Identifier: Apache-2.0
"""PBR texture authoring for the VICTUS starter characters (docs/CHARACTERS.md, ADR-0179).

Every map is a function of the character's rest-pose surface, evaluated per texel through
bake.rasterize, so details sit where the anatomy is (lips, nails, knuckles, the suit's seams)
regardless of how the UV atlas is packed. Outputs per material (linear values in [0, 1]):

  base colour (sRGB-encoded on write), alpha (Groom only), tangent-space normal (from a height
  field in metres), roughness, metallic, ambient occlusion (Cycles bake, multiplied with the
  authored cavity term) and emissive (Suit piping only).

Nothing here is a placeholder: values come from measured ranges where available (skin albedo and
roughness: Weyrich et al. 2006, "Analysis of Human Faces using a Measurement-Based Skin
Reflectance Model"; Donner & Jensen 2006 for melanin/haemoglobin tinting; hair fibre diameters
60-100 um, eyelash lengths 7-11 mm upper / 5-7 mm lower) and procedural detail otherwise.
"""
from __future__ import annotations

import math

import numpy as np

import bake
import fit
from bake import band, dist_to, fbm, soft, value_noise, worley

# ---- landmarks -------------------------------------------------------------------------


class Landmarks:
    def __init__(self, body: fit.Body, parts):
        J = {n: (np.asarray(h), np.asarray(t)) for n, _, h, t in body.joints}
        self.J = J
        self.eL, self.eR = J["Eye.L"][0], J["Eye.R"][0]
        self.eye_mid = 0.5 * (self.eL + self.eR)
        mouth = next(p for p in parts if p.name == "Mouth")
        teeth = mouth.q.v[mouth.attrs["label"][:, 0] < 2]
        self.mouth = np.array([0.0, teeth[:, 1].min() - 0.004, np.median(teeth[:, 2])])
        skin = next(p for p in parts if p.name == "Skin").q.v
        mid = skin[(np.abs(skin[:, 0]) < 0.006) & (skin[:, 2] < self.eye_mid[2]) & (skin[:, 2] > self.eye_mid[2] - 0.07)]
        self.nose = mid[np.argmin(mid[:, 1])]
        chin = skin[(np.abs(skin[:, 0]) < 0.006) & (skin[:, 2] < self.mouth[2] - 0.02) & (skin[:, 2] > self.mouth[2] - 0.07)]
        self.chin = chin[np.argmin(chin[:, 1])]
        self.collar = J["Neck"][0][2] + 0.35 * (J["Head"][0][2] - J["Neck"][0][2])
        self.hands = {}
        for t, side in (("L", 1.0), ("R", -1.0)):
            i1, p1 = J[f"Index1.{t}"][0], J[f"Pinky1.{t}"][0]
            wr = J[f"Hand.{t}"][0]
            m1 = J[f"Middle1.{t}"][0]
            n = np.cross(i1 - p1, m1 - wr)
            n /= np.linalg.norm(n)
            if n[0] * side < 0:
                n = -n
            self.hands[t] = {"dorsal": n, "wrist": wr}


def skull_axis(L):
    return L.eye_mid + np.array([0.0, 0.085, 0.0])


# ---- skin ------------------------------------------------------------------------------


def smooth_(t):
    t = np.clip(t, 0, 1)
    return t * t * (3 - 2 * t)


def skin_maps(tx, P, N, L: Landmarks, style, seed):
    n = len(P)
    rel = P - L.eye_mid
    face_front = np.clip(-N[:, 1], 0, 1)
    on_head = P[:, 2] > L.collar - 0.03

    # Masks.
    m = L.mouth
    ls = getattr(style, "lip_size", 1.0)
    dx = (P[:, 0] - m[0]) / (0.026 * ls)
    up = np.clip((P[:, 2] - m[2]) / (0.0105 * ls), 0, None)
    dn = np.clip((m[2] - P[:, 2]) / (0.0125 * ls), 0, None)
    # Upper lip outline: a cupid's bow dips at the philtrum.
    bow = 1 - 0.18 * np.exp(-(dx * 6) ** 2)
    r_lip = np.sqrt(dx ** 2 + (up / bow) ** 2 + dn ** 2)
    lips = soft(r_lip, 0.82, 1.0) * (P[:, 1] < m[1] + 0.012) * on_head
    nose_tip = soft(dist_to(P, L.nose + [0, 0.004, 0.002]), 0.006, 0.02)
    cheeks = sum(soft(dist_to(P, L.eye_mid + [s * 0.036, -0.005, -0.03]), 0.006, 0.03) for s in (1, -1)) * face_front
    ear_band = (np.abs(P[:, 0]) > 0.062) & (rel[:, 2] > -0.05) & (rel[:, 2] < 0.022) & \
               (P[:, 1] > L.eye_mid[1] + 0.06) & (P[:, 1] < L.eye_mid[1] + 0.13)
    ears = ear_band.astype(float)
    eye_soc = sum(soft(dist_to(P, e + [0, 0.006, -0.002]), 0.012, 0.024) for e in (L.eL, L.eR))
    brow_ridge = sum(soft(dist_to(P, e + [0, -0.004, 0.022]), 0.004, 0.02) for e in (L.eL, L.eR))
    forehead = band(rel[:, 2], 0.03, 0.065, 0.01) * soft(np.abs(P[:, 0]), 0.035, 0.06) * face_front
    jaw_beard = band(P[:, 2], L.chin[2] - 0.035, L.eye_mid[2] - 0.038, 0.012) * on_head
    az = np.degrees(np.abs(np.arctan2(P[:, 0] - skull_axis(L)[0], -(P[:, 1] - skull_axis(L)[1]))))
    jaw_beard *= np.clip((100 - az) / 15, 0, 1)
    # Keep the cheek tops, the nose and the lips clean.
    jaw_beard *= 1 - soft(dist_to(P, L.nose), 0.012, 0.025)
    jaw_beard *= 1 - lips
    cheek_line = L.eye_mid[2] - 0.032 - 0.02 * np.clip((np.abs(P[:, 0]) - 0.02) / 0.04, 0, 1)
    jaw_beard *= np.clip((cheek_line - P[:, 2]) / 0.008, 0, 1)
    under_chin = P[:, 2] < L.chin[2] - 0.035
    jaw_beard = np.where(under_chin, band(P[:, 2], L.collar + 0.01, L.chin[2], 0.01) * (P[:, 1] < L.eye_mid[1] + 0.06), jaw_beard)

    nails = np.zeros(n)
    knuckle = np.zeros(n)
    palm = np.zeros(n)
    for t in ("L", "R"):
        dors = L.hands[t]["dorsal"]
        for f in ("Thumb", "Index", "Middle", "Ring", "Pinky"):
            h3, tip = L.J[f"{f}3.{t}"]
            ax = tip - h3
            ln = np.linalg.norm(ax)
            ax /= ln
            u = (P - h3) @ ax / ln  # 0 at the distal joint, 1 at the tip
            lat = P - h3 - np.outer((P - h3) @ ax, ax)
            dist_axis = np.linalg.norm(lat, axis=1)
            d_dir = dors if f != "Thumb" else _thumb_dorsal(L, t)
            facing = (lat @ d_dir) / np.maximum(dist_axis, 1e-9)
            nail = band(u, 0.42, 1.02, 0.06) * np.clip((facing - 0.35) / 0.25, 0, 1) * (dist_axis < 0.012)
            nails = np.maximum(nails, nail)
            for j in (1, 2, 3):
                hj = L.J[f"{f}{j}.{t}"][0]
                k = soft(dist_to(P, hj), 0.003, 0.009)
                knuckle = np.maximum(knuckle, k * np.clip((P - hj) @ d_dir / 0.004, 0, 1))
        wr = L.hands[t]["wrist"]
        near = dist_to(P, wr) < 0.13
        palm = np.maximum(palm, near * np.clip(-(N @ dors), 0, 1))

    # Height (metres).
    pore_fine = worley(P, 0.00055, seed)
    pore_big = worley(P, 0.0009, seed + 1)
    pores = (np.clip(0.35 - pore_fine, 0, 1) / 0.35) ** 2
    pores_big = (np.clip(0.3 - pore_big, 0, 1) / 0.3) ** 2
    pore_amt = 0.35 + 0.65 * np.clip(nose_tip + cheeks + forehead * 0.6, 0, 1)
    h = -(pores * 0.000022 + pores_big * 0.00002 * pore_amt) * (1 - nails) * (1 - lips * 0.6)
    # Fine crossed skin lines (dermatoglyphic micro-relief).
    g1 = np.abs(value_noise(P * np.array([1, 1, 3.0]), 900, seed + 2))
    g2 = np.abs(value_noise(P * np.array([3.0, 1, 1]), 900, seed + 3))
    h -= (np.clip(0.12 - g1, 0, 1) + np.clip(0.12 - g2, 0, 1)) * 0.00008 * (1 - nails)
    # Forehead lines, crow's feet, nasolabial fold, lip lines, knuckle creases.
    fl = 0.5 + 0.5 * np.cos(2 * np.pi * (rel[:, 2] - 0.036) / 0.0085 + 3 * value_noise(P, 60, seed + 4))
    h -= forehead * fl ** 8 * 0.000035
    for e, s in ((L.eL, 1), (L.eR, -1)):
        d = P - (e + np.array([s * 0.017, 0.006, -0.002]))
        ang = np.arctan2(d[:, 2], np.abs(d[:, 0]) + 1e-9)
        crow = soft(np.linalg.norm(d, axis=1), 0.002, 0.014) * (d[:, 0] * s > 0)
        h -= crow * (0.5 + 0.5 * np.cos(ang * 14)) ** 6 * 0.00003
    lip_lines = lips * (0.5 + 0.5 * np.cos(P[:, 0] * 2 * np.pi / 0.0011 + 2 * value_noise(P, 400, seed + 5))) ** 4
    h -= lip_lines * 0.00002
    kn = knuckle * (0.5 + 0.5 * np.cos(2 * np.pi * value_noise(P, 900, seed + 6) * 2)) ** 3
    h -= kn * 0.00005
    # Nail plate: smooth, slightly raised with a curved edge.
    h += nails * 0.00012

    # Base colour (linear).
    base = np.array(style.skin)
    mel = fbm(P, 60, 4, seed + 7)
    col = base[None, :] * (1 + 0.08 * mel[:, None])
    red = np.array([1.12, 0.86, 0.84])
    redness = np.clip(0.55 * cheeks + 0.6 * nose_tip + 0.5 * ears + 0.25 * knuckle, 0, 1)
    col *= 1 + (red - 1)[None, :] * redness[:, None]
    col *= (1 - 0.10 * eye_soc)[:, None] * np.array([1.0, 0.97, 1.0])
    col = col * (1 - lips[:, None]) + np.array(style.lips)[None, :] * (1 + 0.05 * mel[:, None]) * lips[:, None]
    col *= 1 - 0.04 * brow_ridge[:, None]
    # Palms and nail beds are lighter (less melanin).
    pale = np.array([1.35, 1.22, 1.18]) if style.skin[0] < 0.3 else np.array([1.06, 1.03, 1.03])
    col *= 1 + (pale - 1)[None, :] * (palm * 0.8)[:, None]
    nail_col = np.minimum(base * np.array([1.6, 1.5, 1.55]) + 0.08, 0.75)
    col = col * (1 - nails[:, None]) + nail_col[None, :] * nails[:, None]
    if style.stubble > 0:
        foll = (np.clip(0.28 - worley(P, 0.0006, seed + 8), 0, 1) / 0.28)
        shadow = jaw_beard * style.stubble
        col *= (1 - shadow * (0.18 + 0.5 * foll))[:, None] * np.array([1.0, 1.0, 1.04])
        h -= shadow * foll * 0.00001
    if style.freckles > 0:
        fr_mask = np.clip(soft(dist_to(P, L.nose), 0.0, 0.05) + cheeks, 0, 1) * face_front
        fr = np.clip(0.18 - worley(P, 0.0035, seed + 9), 0, 1) / 0.18
        fr *= value_noise(P, 400, seed + 10) > 0.1
        col *= (1 - fr * fr_mask * style.freckles * 0.35)[:, None] * np.array([1.0, 0.96, 0.92])

    mk = getattr(style, "makeup", 0.0)
    liner = np.zeros(n)
    shadow_e = np.zeros(n)
    if mk > 0:
        # The sheet's close-up: a dark lash line (thicker on the upper lid, winged out a little),
        # soft warm-brown shadow on the lids, rose blush high on the cheeks and lips deeper at
        # the centre. The lid margin is the skin closest to each eyeball's centre.
        for e, s_ in ((L.eL, 1.0), (L.eR, -1.0)):
            r3 = dist_to(P, e)
            near = (r3 < 0.03) & (P[:, 1] < e[1] - 0.005) & (face_front > 0.25)
            if not near.any():
                continue
            rmin = np.percentile(r3[near], 0.5)
            dz = P[:, 2] - e[2]
            out = (P[:, 0] - e[0]) * s_
            upper = smooth_((dz + 0.0015) / 0.003)
            lower = 1 - upper
            wing = np.clip(out / 0.016, 0, 1) ** 3
            lw = 0.0018 + 0.0012 * wing
            line_ = smooth_(1 - (r3 - rmin - lw * 0.5) / (lw * 0.5)) * (upper + 0.35 * lower) * near
            liner = np.maximum(liner, line_)
            shadow_e = np.maximum(shadow_e, soft(r3 - rmin, 0.002, 0.011) * upper * near * (1 - line_))
        col *= (1 - mk * (0.78 * liner))[:, None]
        col *= (1 - mk * 0.30 * shadow_e[:, None] * np.array([0.55, 0.85, 1.0]))
        blush = sum(soft(dist_to(P, L.eye_mid + [s * 0.040, -0.008, -0.026]), 0.004, 0.030) for s in (1, -1)) * face_front
        col *= 1 + mk * 0.22 * blush[:, None] * np.array([0.6, -0.25, -0.15])
        lip_core = lips * soft(np.abs(dx), 0.0, 0.8)
        col *= (1 - mk * 0.22 * lip_core[:, None] * np.array([0.3, 1.0, 0.8]))

    # Roughness.
    rough = 0.52 - 0.08 * np.clip(nose_tip * 1.2 + forehead * 0.6, 0, 1) - 0.12 * lips + 0.04 * mel
    rough = rough * (1 - nails) + 0.24 * nails
    rough += pores * 0.06
    rough -= mk * (0.10 * lips + 0.05 * liner)  # glossy lips, a satin liner
    if style.stubble > 0:
        rough += jaw_beard * style.stubble * 0.08
    cavity = 1 - 0.3 * np.clip(-h / 0.00008, 0, 1)
    return {"color": np.clip(col, 0, 1), "rough": np.clip(rough, 0.05, 1), "metal": np.zeros(n),
            "height": h, "cavity": cavity}


def _thumb_dorsal(L, t):
    h1, h3 = L.J[f"Thumb1.{t}"][0], L.J[f"Thumb3.{t}"][0]
    ax = h3 - h1
    ax /= np.linalg.norm(ax)
    d = L.hands[t]["dorsal"]
    # The thumbnail faces away from the palm, rotated ~70 degrees toward the index finger side.
    i1 = L.J[f"Index1.{t}"][0]
    toward = i1 - h1
    toward -= ax * (toward @ ax)
    toward /= np.linalg.norm(toward)
    out = -toward * math.cos(math.radians(20)) + d * math.sin(math.radians(20))
    out -= ax * (out @ ax)
    return out / np.linalg.norm(out)


# ---- suit ------------------------------------------------------------------------------


def suit_maps(tx, P, N, L: Landmarks, style, cuff, seed):
    """The VICTUS undersuit: matte technical knit, smoother bonded panels on chest, shoulders,
    elbows and knees, taped seams with twin stitching and thin emissive piping."""
    n = len(P)
    J = L.J
    hips, chest, neck = J["Hips"][0], J["Chest"][0], J["Neck"][0]
    # Side seams: where the rest normal turns sideways (torso and legs), plus the inseam.
    nx = np.abs(N[:, 0])
    torso = (P[:, 2] > hips[2] - 0.05) & (P[:, 2] < neck[2]) & (np.abs(P[:, 0]) < 0.22)
    legs = P[:, 2] < hips[2] - 0.05
    seam_side = np.exp(-((nx - 0.82) / 0.035) ** 2) * (torso | legs)
    # Waist band.
    wz = hips[2] + 0.06
    seam_waist = np.exp(-((P[:, 2] - wz) / 0.0025) ** 2) * torso
    # Chest plastron: front torso panel with a V toward the collar.
    front = N[:, 1] < -0.25
    cz0, cz1 = chest[2] - 0.12, neck[2] - 0.01
    v_edge = 0.11 - (P[:, 2] - cz0) * 0.35
    in_chest = front & (P[:, 2] > cz0) & (P[:, 2] < cz1) & (np.abs(P[:, 0]) < v_edge)
    chest_d = np.minimum(np.minimum(v_edge - np.abs(P[:, 0]), P[:, 2] - cz0), cz1 - P[:, 2])
    chest_edge = np.exp(-(chest_d / 0.0025) ** 2) * front * (P[:, 2] > cz0 - 0.01) * (P[:, 2] < cz1 + 0.01) * \
        (np.abs(P[:, 0]) < v_edge + 0.01)
    # Shoulder, elbow and knee pads.
    pads = np.zeros(n)
    pad_edge = np.zeros(n)
    for tt in ("L", "R"):
        for joint, radius in ((f"UpperArm.{tt}", 0.065), (f"LowerArm.{tt}", 0.045), (f"LowerLeg.{tt}", 0.06)):
            c = J[joint][0].copy()
            if joint.startswith("UpperArm"):
                c = c + np.array([0, 0, 0.02])
            if joint.startswith("LowerArm"):
                c = c + np.array([0, 0.03, 0])
            if joint.startswith("LowerLeg"):
                c = c + np.array([0, -0.04, 0])
            d = dist_to(P, c)
            pads = np.maximum(pads, soft(d, radius - 0.003, radius))
            pad_edge = np.maximum(pad_edge, np.exp(-((d - radius) / 0.002) ** 2))
    panel = np.clip(in_chest.astype(float) + pads, 0, 1)
    seams = np.clip(seam_side + seam_waist + chest_edge + pad_edge, 0, 1)
    # Twin stitch rows either side of each seam (dash pattern along the surface).
    dash = 0.5 + 0.5 * np.sin(2 * np.pi * (P @ np.array([0.31, 0.53, 0.79])) / 0.003)
    stitch = np.clip(seams * 3 - 1.4, 0, 1) * (dash > 0.25)

    # Piping: emissive lines along the chest edge and down the outer arm and leg.
    arm_line = np.zeros(n)
    for tt, s in (("L", 1), ("R", -1)):
        a, b = J[f"UpperArm.{tt}"][0], J[f"Hand.{tt}"][0]
        ax = (b - a) / np.linalg.norm(b - a)
        u = (P - a) @ ax
        lat = P - a - np.outer(u, ax)
        up = np.array([0, 0, 1.0]) - ax * ax[2]
        up /= np.linalg.norm(up)
        cosang = (lat @ up) / np.maximum(np.linalg.norm(lat, axis=1), 1e-9)
        arm_line = np.maximum(arm_line, np.exp(-((cosang - 0.98) / 0.012) ** 2) * (u > 0.05) * (u < np.linalg.norm(b - a) - 0.04)
                              * (P[:, 0] * s > 0))
    leg_line = np.exp(-((nx - 0.97) / 0.01) ** 2) * legs * (P[:, 2] > 0.12)
    piping = np.clip(arm_line + leg_line, 0, 1)
    chest_pipe = np.exp(-((chest_d - 0.006) / 0.0011) ** 2) * front * (np.abs(P[:, 0]) < v_edge + 0.002) * \
        (P[:, 2] > cz0 - 0.002) * (P[:, 2] < cz1 + 0.002)
    # The chest outline is a contrasting bonded tape, not a light: emissive piping stays on the
    # limbs only (a lit V on the chest read as a decal at gameplay distance).
    seams = np.clip(seams + 0.6 * chest_pipe, 0, 1)

    # Knit micro-structure in texel space (fine weave) plus 3D fibre noise.
    ij = tx.ij.astype(float)
    weave = np.sin(ij[:, 0] * np.pi / 2.0) * np.sin(ij[:, 1] * np.pi / 2.0)
    fibres = fbm(P, 1500, 2, seed)
    h = (weave * 0.000006 + fibres * 0.000004) * (1 - panel)
    # Panels: slightly raised, with a quilted rib on the pads.
    rib = 0.5 + 0.5 * np.cos(2 * np.pi * P[:, 2] / 0.012)
    h += panel * 0.0004 + pads * rib ** 4 * 0.00025
    h -= seams * 0.00035
    h += stitch * 0.00012

    base = np.array(style.suit)
    trim = np.array(style.trim)
    mottle = fbm(P, 30, 3, seed + 1)
    col = base[None, :] * (1 + 0.06 * mottle[:, None] + 0.04 * weave[:, None])
    col = np.where(panel[:, None] > 0.5, base[None, :] * 1.6 * (1 + 0.03 * mottle[:, None]), col)
    col = col * (1 - stitch[:, None]) + trim[None, :] * stitch[:, None] * 0.6
    tape = np.clip(chest_pipe * 1.5 - 0.2, 0, 1)
    col = col * (1 - tape[:, None]) + trim[None, :] * 0.5 * tape[:, None]
    col = col * (1 - cuff[:, None]) + (base * 1.3)[None, :] * cuff[:, None]
    piping_core = np.clip(piping * 1.4 - 0.2, 0, 1)
    col = col * (1 - piping_core[:, None]) + np.array(style.glow)[None, :] * 0.25 * piping_core[:, None]
    rough = 0.78 + 0.05 * mottle - 0.30 * panel - 0.2 * piping_core - 0.1 * cuff
    metal = np.zeros(n)
    emissive = np.array(style.glow)[None, :] * piping_core[:, None]
    cavity = 1 - 0.35 * seams
    return {"color": np.clip(col, 0, 1), "rough": np.clip(rough, 0.05, 1), "metal": metal, "height": h,
            "cavity": cavity, "emissive": np.clip(emissive, 0, 1)}


# ---- equipment -------------------------------------------------------------------------


def gear_maps(tx, P, N, L: Landmarks, style, kind, cuff, seed):
    """Boots (kind 0): pebbled leather uppers, a moulded rubber sole with lug tread and a
    stitched welt; gloves (1): fine-grain synthetic leather with a textured palm and knuckle
    panel; belt (2): woven nylon webbing with a brushed-metal buckle; backpack (3): coated
    ripstop with taped panel seams, a zip track and two webbing straps; pack shoulder straps (4)."""
    n = len(P)
    J = L.J
    k = np.rint(kind).astype(np.int64)
    boots, gloves, belt, pack, straps_k = (k == 0), (k == 1), (k == 2), (k == 3), (k == 4)
    gear = np.array(style.gear)
    web = np.array(style.webbing)
    col = np.tile(gear, (n, 1))
    rough = np.full(n, 0.6)
    metal = np.zeros(n)
    h = np.zeros(n)
    cavity = np.ones(n)
    ij = tx.ij.astype(float)
    mottle = fbm(P, 25, 3, seed + 40)

    # Boots.
    if boots.any():
        grain = worley(P[boots], 0.0016, seed + 41)
        sole_top = 0.028
        z = P[boots, 2]
        sole = soft(z - sole_top, -0.0015, 0.0015)
        tread_cell = worley(P[boots] * np.array([1, 1, 0.0]), 0.009, seed + 42)
        tread = np.clip((tread_cell - 0.35) * 4, 0, 1) * (N[boots, 2] < -0.3)
        lug = sole * (0.0008 * tread + 0.0003 * (0.5 + 0.5 * np.sin(2 * np.pi * z / 0.004)) * (N[boots, 2] > -0.3))
        welt = np.exp(-((z - sole_top) / 0.0012) ** 2)
        stitch = np.exp(-((z - sole_top - 0.004) / 0.0005) ** 2) * \
            (np.sin(2 * np.pi * (P[boots] @ np.array([0.7, 0.7, 0.1])) / 0.004) > -0.2)
        # Toe and heel caps: smoother panels.
        foot_l, foot_r = J["Toe.L"][0], J["Toe.R"][0]
        toe = np.maximum(soft(dist_to(P[boots], foot_l), 0.035, 0.04), soft(dist_to(P[boots], foot_r), 0.035, 0.04))
        upper_h = (grain ** 2) * 0.00006 * (1 - toe) - welt * 0.0004 + stitch * 0.0001
        h[boots] = upper_h * (1 - sole) + lug
        col[boots] = gear * (1 + 0.08 * mottle[boots, None]) * (1 - 0.15 * toe[:, None])
        rubber = np.array([0.018, 0.017, 0.016])
        col[boots] = col[boots] * (1 - sole[:, None]) + rubber * sole[:, None]
        col[boots] = col[boots] * (1 - stitch[:, None]) + (web * 2.5) * stitch[:, None]
        rough[boots] = (0.55 - 0.15 * toe + 0.05 * mottle[boots]) * (1 - sole) + 0.85 * sole
        cavity[boots] = 1 - 0.4 * welt - 0.25 * tread * sole

    # Gloves.
    if gloves.any():
        Pg = P[gloves]
        grain = worley(Pg, 0.0009, seed + 43)
        palm = np.zeros(len(Pg))
        knuck = np.zeros(len(Pg))
        for t in ("L", "R"):
            wr, m1 = J[f"Hand.{t}"][0], J[f"Middle1.{t}"][0]
            side = dist_to(Pg, wr) < 0.25
            dors = L.hands[t]["dorsal"]
            palm = np.maximum(palm, side * np.clip((-(N[gloves] @ dors) - 0.2) * 2, 0, 1))
            kc = wr + (m1 - wr) * 1.02 + dors * 0.01
            knuck = np.maximum(knuck, side * soft(dist_to(Pg, kc), 0.03, 0.034) * np.clip((N[gloves] @ dors) * 2, 0, 1))
        dots = np.clip(1.0 - worley(Pg, 0.0025, seed + 44) * 2.2, 0, 1)
        rib = 0.5 + 0.5 * np.cos(2 * np.pi * (Pg @ np.array([0.0, 1.0, 0.3])) / 0.006)
        edge = np.exp(-(((knuck - 0.5) / 0.15) ** 2))
        h[gloves] = grain ** 2 * 0.00003 + palm * dots * 0.00025 + knuck * (0.0005 + rib ** 3 * 0.0002) - edge * 0.0002
        col[gloves] = gear * 1.15 * (1 + 0.06 * mottle[gloves, None])
        col[gloves] = col[gloves] * (1 - palm[:, None]) + (gear * 0.7) * palm[:, None]
        col[gloves] = col[gloves] * (1 - knuck[:, None]) + (web * 0.8) * knuck[:, None]
        rough[gloves] = 0.45 + 0.35 * palm - 0.1 * knuck
        cavity[gloves] = 1 - 0.3 * edge

    # Belt.
    if belt.any():
        Pb = P[belt]
        hips = J["Hips"][0]
        z0, z1 = hips[2] + 0.04, hips[2] + 0.085
        u = (Pb[:, 2] - z0) / (z1 - z0)
        weave = 0.5 + 0.5 * np.sin(ij[belt, 1] * np.pi / 1.5) * np.sin(ij[belt, 0] * np.pi / 3.0)
        ribs = 0.5 + 0.5 * np.cos(2 * np.pi * Pb[:, 2] / 0.0018)
        edge = np.exp(-(u / 0.06) ** 2) + np.exp(-((1 - u) / 0.06) ** 2)
        # Buckle: a rounded-rectangle plate at the front centre.
        bx, bz = np.abs(Pb[:, 0]) / 0.032, np.abs(Pb[:, 2] - 0.5 * (z0 + z1)) / 0.026
        front = N[belt, 1] < -0.5
        r = (np.maximum(bx, 0) ** 6 + np.maximum(bz, 0) ** 6) ** (1 / 6)
        plate = soft(r, 0.96, 1.0) * front
        frame = plate * (1 - soft(r, 0.70, 0.74))
        bar = plate * np.exp(-((Pb[:, 0]) / 0.003) ** 2) * (bz < 0.7)
        h[belt] = (weave * 0.00002 + ribs * 0.00004) * (1 - plate) - edge * 0.00012 + plate * 0.0012 + frame * 0.0006 + bar * 0.0004
        col[belt] = web * (1 + 0.1 * (weave[:, None] - 0.5) + 0.05 * mottle[belt, None])
        metal_col = np.array([0.56, 0.55, 0.52])
        col[belt] = col[belt] * (1 - plate[:, None]) + metal_col * plate[:, None]
        brushed = fbm(Pb * np.array([1.0, 1.0, 40.0]), 400, 2, seed + 45)
        rough[belt] = 0.8 * (1 - plate) + (0.32 + 0.06 * brushed - 0.1 * frame) * plate
        metal[belt] = plate
        cavity[belt] = 1 - 0.3 * edge - 0.4 * plate * (1 - frame) * (r > 0.66) * (r < 0.78)

    # Backpack.
    if pack.any():
        Pp = P[pack]
        Np = N[pack]
        ax = np.abs(Np)
        dominant = ax.argmax(1)
        # Panel tape where the dominant axis changes (the rounded box edges).
        second = np.sort(ax, 1)[:, -2]
        tape = np.exp(-(((ax.max(1) - second) - 0.0) / 0.12) ** 2)
        rip = (np.abs(np.sin(ij[pack, 0] * np.pi / 6)) > 0.97) | (np.abs(np.sin(ij[pack, 1] * np.pi / 6)) > 0.97)
        weave = np.sin(ij[pack, 0] * np.pi / 1.0) * np.sin(ij[pack, 1] * np.pi / 1.0)
        c = Pp.mean(0)
        back_face = (dominant == 1) & (Np[:, 1] > 0)
        zip_line = back_face * np.exp(-((Pp[:, 2] - (c[2] + 0.07)) / 0.0025) ** 2) * (np.abs(Pp[:, 0] - c[0]) < 0.10)
        teeth = zip_line * (np.sin(2 * np.pi * Pp[:, 0] / 0.002) > 0)
        straps = np.zeros(len(Pp))
        for s in (-1, 1):
            straps = np.maximum(straps, band(np.abs(Pp[:, 0] - (c[0] + s * 0.07)), -1, 0.018, 0.002) * back_face)
        strap_rib = 0.5 + 0.5 * np.cos(2 * np.pi * Pp[:, 2] / 0.0018)
        h[pack] = weave * 0.00002 + rip * 0.00008 - tape * 0.0003 + straps * (0.0015 + strap_rib * 0.00004) - zip_line * 0.0004 + teeth * 0.0002
        col[pack] = gear * 1.25 * (1 + 0.06 * mottle[pack, None])
        col[pack] = col[pack] * (1 - straps[:, None]) + web * straps[:, None]
        col[pack] = col[pack] * (1 - tape[:, None] * 0.6) + (web * 0.6) * tape[:, None] * 0.6
        col[pack] = col[pack] * (1 - zip_line[:, None]) + np.array([0.02, 0.02, 0.02]) * zip_line[:, None]
        rough[pack] = 0.7 - 0.15 * tape + 0.1 * straps - 0.3 * teeth
        metal[pack] = teeth * 0.8
        cavity[pack] = 1 - 0.35 * tape - 0.3 * zip_line

    # Pack shoulder straps: padded webbing with edge binding and a ladder-lock row.
    if straps_k.any():
        Ps = P[straps_k]
        ribs = 0.5 + 0.5 * np.cos(2 * np.pi * (Ps @ np.array([0.0, 0.3, 1.0])) / 0.0018)
        lat = np.abs(np.abs(Ps[:, 0]) - 0.095) / 0.02
        bind = np.clip((lat - 0.8) * 5, 0, 1)
        h[straps_k] = 0.0008 * np.sqrt(np.clip(1 - lat ** 2, 0, 1)) + ribs * 0.00004 - bind * 0.0002
        col[straps_k] = web * (1 + 0.08 * (ribs[:, None] - 0.5)) * (1 - 0.3 * bind[:, None])
        rough[straps_k] = 0.82
        cavity[straps_k] = 1 - 0.25 * bind

    # Rolled lips at the cut edges: tucked under, darker and slightly rougher.
    col = col * (1 - 0.3 * cuff[:, None])
    cavity = cavity * (1 - 0.3 * cuff)
    return {"color": np.clip(col, 0, 1), "rough": np.clip(rough, 0.05, 1), "metal": np.clip(metal, 0, 1),
            "height": h, "cavity": np.clip(cavity, 0, 1)}


# ---- eyes ------------------------------------------------------------------------------


IRIS_DEG = 30.0  # half-angle of an ~11.7 mm iris on a ~24 mm eyeball
PUPIL_DEG = 8.5


def eye_maps(tx, P, N, attrs, style, seed):
    theta = attrs["theta"][:, 0]
    phi = attrs["phi"][:, 0]
    n = len(P)
    iris = soft(theta, IRIS_DEG - 1.0, IRIS_DEG + 0.6)
    pupil = soft(theta, PUPIL_DEG - 0.6, PUPIL_DEG + 0.4)
    limbal = np.exp(-((theta - IRIS_DEG) / 1.6) ** 2)
    rr = np.clip((theta - PUPIL_DEG) / (IRIS_DEG - PUPIL_DEG), 0, 1)
    # Radial crypts and fibres of the iris stroma.
    fib = value_noise(np.stack([np.cos(phi) * 3, np.sin(phi) * 3, rr * 0.4], 1), 18, seed)
    fib2 = value_noise(np.stack([np.cos(phi) * 9, np.sin(phi) * 9, rr * 1.2], 1), 22, seed + 1)
    collarette = np.exp(-((rr - 0.35) / 0.06) ** 2)
    ic = np.array(style.iris)
    iris_col = ic[None, :] * (0.75 + 0.35 * fib[:, None] + 0.25 * fib2[:, None]) * (1 + 0.6 * collarette[:, None]) \
        * (0.8 + 0.4 * rr[:, None])
    sclera = np.array([0.72, 0.68, 0.64])
    # Veins near the edge of the visible sclera and a slight warm tint toward the corners.
    vein = np.clip(np.abs(value_noise(np.stack([np.cos(phi) * 6, np.sin(phi) * 6, theta * 0.02], 1), 10, seed + 2)) * -1 + 0.06, 0, 1) / 0.06
    vein *= np.clip((theta - 45) / 30, 0, 1)
    scl = sclera[None, :] * (1 - 0.15 * np.clip((theta - 40) / 50, 0, 1))[:, None]
    scl = scl * (1 - vein[:, None] * 0.6) + np.array([0.5, 0.12, 0.1])[None, :] * vein[:, None] * 0.6
    col = scl * (1 - iris[:, None]) + iris_col * iris[:, None]
    col *= (1 - 0.65 * limbal * (1 - iris * 0.3))[:, None]
    col = col * (1 - pupil[:, None]) + 0.004 * pupil[:, None]
    rough = np.full(n, 0.06)
    return {"color": np.clip(col, 0, 1), "rough": rough, "metal": np.zeros(n), "height": np.zeros(n),
            "cavity": np.ones(n)}


# ---- mouth ------------------------------------------------------------------------------


def mouth_maps(tx, P, N, attrs, seed):
    lab = np.rint(attrs["label"][:, 0])
    z = attrs["z"][:, 0]
    n = len(P)
    teeth = lab < 2
    upper = lab == 0
    # Gum line: the top band of the upper teeth and the bottom band of the lower teeth.
    zu = z[upper]
    zl = z[lab == 1]
    gum = np.zeros(n)
    if len(zu):
        gum = np.where(upper, np.clip((z - (zu.max() - 0.004)) / 0.002, 0, 1), gum)
    if len(zl):
        gum = np.where(lab == 1, np.clip(((zl.min() + 0.004) - z) / 0.002, 0, 1), gum)
    # Tooth separations from the angle around the dental arch.
    ang = np.arctan2(P[:, 0], -(P[:, 1] - P[:, 1].max()))
    sep = (0.5 + 0.5 * np.cos(ang * 22)) ** 12 * teeth
    tooth = np.array([0.62, 0.58, 0.48])
    gums = np.array([0.42, 0.12, 0.11])
    tongue = np.array([0.40, 0.12, 0.11])
    col = np.where(teeth[:, None], tooth[None, :] * (1 - 0.35 * sep[:, None]), tongue[None, :])
    col = col * (1 - gum[:, None]) + gums[None, :] * gum[:, None]
    pap = np.clip(0.25 - worley(P, 0.0009, seed), 0, 1) / 0.25
    h = np.where(lab == 2, pap * 0.00006, -sep * 0.0002)
    rough = np.where(teeth, 0.22, 0.38) - gum * 0.06
    col *= np.where(lab == 2, 1 + 0.1 * pap, 1.0)[:, None]
    return {"color": np.clip(col, 0, 1), "rough": rough, "metal": np.zeros(n), "height": h, "cavity": np.ones(n)}


# ---- groom ------------------------------------------------------------------------------


def lic(P, flow, cell, steps, step_len, seed):
    """Line-integral convolution of 3D white-ish noise along `flow`: streaks a strand length long."""
    acc = np.zeros(len(P))
    for k in range(-steps, steps + 1):
        acc += value_noise(P + flow * (k * step_len), 1.0 / cell, seed)
    return acc / (2 * steps + 1) * math.sqrt(2 * steps + 1)


def groom_maps(tx, P, N, attrs, style, seed):
    kind = np.rint(attrs["kind"][:, 0])
    n = len(P)
    flow = attrs["flow"]
    fl = np.linalg.norm(flow, axis=1, keepdims=True)
    flow = np.where(fl > 1e-6, flow / np.maximum(fl, 1e-9), np.array([[0, 0, -1.0]]))
    hair = kind == 0
    brow = kind == 1
    lash = kind == 2
    alpha = np.zeros(n)
    h = np.zeros(n)
    col = np.zeros((n, 3))
    hc = np.array(style.hair)

    # Scalp hair: dense strands, fading at the hairline.
    if hair.any():
        Ph, fh = P[hair], flow[hair]
        # Strands about a texel wide (0.25 mm at the 2048 atlas) streaked along the flow, grouped
        # into clumps a few millimetres across; gaps between strands read as depth, not as scalp.
        s1 = lic(Ph, fh, 0.0004, 6, 0.0008, seed)
        s2 = lic(Ph, fh, 0.0014, 5, 0.002, seed + 1)
        strands = np.clip(0.5 + 0.8 * s1 + 0.45 * s2, 0, 1)
        marg = attrs["margin"][hair, 0]
        # (ADR-0252: the bob's cards cover its hairline, so the shell edge is cut clean rather than
        # stippled where the swept fringe exposes it.)
        density = np.clip(marg / {"crop": 0.006, "bob": 0.0012}.get(style.hair_style, 0.004), 0, 1)
        thresh = 1 - density
        a = np.clip((strands - thresh * 0.95) / 0.08, 0, 1)
        a = np.where(density > 0.98, 1.0, a)
        alpha[hair] = a
        h[hair] = strands * 0.00025
        tone = 0.45 + 0.85 * strands + 0.12 * fbm(Ph, 80, 2, seed + 2)
        col[hair] = hc[None, :] * tone[:, None]
    if brow.any():
        Pb, fb = P[brow], flow[brow]
        s = lic(Pb, fb, 0.00012, 6, 0.0006, seed + 3)
        dens = attrs["density"][brow, 0]
        a = np.clip((0.5 + 1.1 * s - (1 - dens) * 1.2) / 0.1, 0, 1) * (dens > 0.02)
        alpha[brow] = a
        h[brow] = s * 0.00003
        col[brow] = hc[None, :] * (0.9 + 0.4 * np.clip(s, 0, 1))[:, None]
    if lash.any():
        s = attrs["s"][lash, 0]
        t = attrs["t"][lash, 0]
        upper = attrs["upper"][lash, 0] > 0.5
        count = np.where(upper, 110.0, 60.0)
        # Lashes fan along the lid; each strand tapers from the root to the tip, longer mid-lid.
        k = s * count
        jitter = value_noise(np.stack([np.floor(k), np.zeros_like(k), np.zeros_like(k)], 1), 1.0, seed + 4)
        length = (0.55 + 0.45 * np.sin(np.pi * np.clip(s, 0, 1))) * (0.85 + 0.15 * jitter)
        bend = 0.25 * t * t
        frac = np.abs(((k + bend + 0.5 + 0.15 * jitter) % 1.0) - 0.5)
        width = 0.22 * (1 - t / np.maximum(length, 1e-3)) + 0.02
        a = np.clip((width - frac) / 0.06, 0, 1) * (t < length) * (t > 0.02)
        alpha[lash] = a
        col[lash] = hc[None, :] * 0.8
    # Hair is a rough dielectric at this scale: the strand normals carry the sheen, so the base
    # roughness stays high and rises in the gaps between strands.
    rough = np.where(lash, 0.45, np.where(hair, 0.72 - 0.16 * np.clip(h / 0.00025, 0, 1), 0.6))
    return {"color": np.clip(col, 0, 1), "rough": rough, "metal": np.zeros(n), "height": h, "alpha": alpha,
            "cavity": np.ones(n)}
