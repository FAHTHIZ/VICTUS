# SPDX-License-Identifier: Apache-2.0
"""The VICTUS Frame undersuit's panel layout (mannequin_materials.frame_maps, docs/CHARACTERS.md).

The undersuit is a moulded graphite bodysuit. Its panels follow the muscles they cover, after the
ORION & NOVA design sheet's undersuit layer: the pectorals, a rectus grid, the obliques and
serratus, the shoulder blades and spine, the glutes, the deltoids, biceps and triceps, the
forearms, the quadriceps and the calves. Each panel is a `plates.Plate` outline on the same charts
the armour uses; the bake turns its signed distance into a recessed seam along the outline and a
soft moulded crown inside it (the muscle relief survives at texture resolution even where the
mesh is smooth). Panels flagged `glow` carry a thin light line in their seam (cyan on Orion,
violet on Nova).
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from plates import Chart, Plate, Torso, _unit, mirrored


@dataclass
class Panel:
    plate: Plate
    glow: float = 0.0  # light-line strength in the seam (0 none)
    relief: float = 0.0010  # crown height (metres) of the moulded panel
    crown: float = 0.022  # distance from the seam to full relief (metres)
    seam: bool = True  # False: a light line only (an open polyline, no seam or relief)


def panels(J, torso: Torso, key):
    nova = key == "nova"
    hips, neck = J["Hips"][0], J["Neck"][0]
    zh, zn = hips[2], neck[2]

    def z(t):
        return zh + (zn - zh) * t

    F = Chart("front", torso=torso)
    B = Chart("back", torso=torso)
    gate = (hips - np.array([0, 0, 0.16]), neck + np.array([0, 0, 0.03]), 0.30)
    out = []

    def add(name, chart, outline, mirror=True, g=None, **kw):
        p = Plate(name, chart, outline, round=kw.pop("round", 0.008), gate=gate if g is None else g)
        out.append(Panel(p, **kw))
        if mirror:
            out.append(Panel(mirrored(p), **kw))

    def line(name, chart, pts, strength=1.0, mirror=True, g=None):
        """A light line along an open polyline (a zero-area polygon: its distance is the line's)."""
        pts = list(pts)
        add(name, chart, pts + pts[::-1][1:-1], mirror=mirror, g=g, glow=strength, relief=0.0, seam=False, round=0.0)

    # ---- front ----
    if not nova:
        add("Pec.L", F, [(0.010, z(0.80)), (0.010, z(0.95)), (0.07, z(0.975)), (0.135, z(0.95)), (0.165, z(0.88)),
                         (0.15, z(0.80)), (0.10, z(0.755)), (0.045, z(0.76))], relief=0.0016, crown=0.035)
        rows = ((0.62, 0.725), (0.50, 0.605), (0.38, 0.485), (0.24, 0.365))
        for i, (a, b) in enumerate(rows):
            w = 0.068 - 0.004 * i
            add(f"Rectus{i}.L", F, [(0.009, z(a)), (w, z(a + 0.01)), (w + 0.004, z(b)), (0.009, z(b))],
                relief=0.0012, crown=0.016, round=0.010)
        add("Oblique.L", F, [(0.082, z(0.73)), (0.125, z(0.76)), (0.17, z(0.70)), (0.175, z(0.40)), (0.15, z(0.22)),
                             (0.08, z(0.20)), (0.086, z(0.40))], relief=0.0010, crown=0.02)
        # Light lines down the sides of the abdomen curving into the inguinal V, and under the pecs.
        line("AbLine.L", F, [(0.084, z(0.745)), (0.080, z(0.55)), (0.078, z(0.36)), (0.066, z(0.20)), (0.035, z(0.04))])
        line("PecLine.L", F, [(0.165, z(0.86)), (0.15, z(0.795)), (0.10, z(0.75))], 0.8)
        add("Collar.L", F, [(0.0, z(0.975)), (0.07, z(0.99)), (0.15, z(0.965)), (0.17, z(1.08)), (0.0, z(1.10))],
            relief=0.0005)
    else:
        # Bust: a rounded cup panel, a centre keel, a cinched corset below with light lines.
        add("Bust.L", F, [(0.010, z(0.74)), (0.010, z(0.93)), (0.06, z(0.965)), (0.12, z(0.94)), (0.145, z(0.86)),
                          (0.13, z(0.76)), (0.08, z(0.715)), (0.035, z(0.72))], relief=0.0014, crown=0.035, round=0.02)
        add("Corset.L", F, [(0.009, z(0.69)), (0.07, z(0.67)), (0.125, z(0.70)), (0.112, z(0.50)), (0.12, z(0.30)),
                            (0.13, z(0.18)), (0.009, z(0.22))], relief=0.0008, crown=0.03, round=0.012)
        # Light lines: under the bust, and an hourglass down the corset converging to the hips.
        line("UnderBust.L", F, [(0.012, z(0.72)), (0.05, z(0.70)), (0.10, z(0.705)), (0.135, z(0.75))], 0.9)
        line("Waist.L", F, [(0.125, z(0.70)), (0.108, z(0.52)), (0.10, z(0.40)), (0.085, z(0.24)), (0.04, z(0.06))])
        for i, (a, b) in enumerate(((0.52, 0.64), (0.38, 0.50), (0.25, 0.36))):
            add(f"Rib{i}.L", F, [(0.022, z(a)), (0.085, z(a + 0.02)), (0.085, z(b + 0.02)), (0.022, z(b))],
                relief=0.0007, crown=0.012, round=0.012)
        add("Flank.L", F, [(0.13, z(0.74)), (0.16, z(0.72)), (0.15, z(0.45)), (0.165, z(0.16)), (0.135, z(0.16)),
                           (0.12, z(0.45))], relief=0.0006, crown=0.01)
        add("Collar.L", F, [(0.0, z(0.96)), (0.065, z(0.985)), (0.14, z(0.96)), (0.16, z(1.08)), (0.0, z(1.10))],
            relief=0.0005)
    # ---- back ----
    w = 0.88 if nova else 1.0
    add("Trap.L", B, [(0.008, z(0.98)), (0.09 * w, z(1.0)), (0.17 * w, z(0.94)), (0.10 * w, z(0.86)), (0.008, z(0.80))],
        relief=0.0010, crown=0.025)
    add("Blade.L", B, [(0.025, z(0.80)), (0.11 * w, z(0.88)), (0.165 * w, z(0.86)), (0.17 * w, z(0.66)),
                       (0.14 * w, z(0.50)), (0.05, z(0.56))], relief=0.0014, crown=0.035)
    add("Erector.L", B, [(0.008, z(0.74)), (0.04, z(0.70)), (0.05, z(0.30)), (0.008, z(0.22))], relief=0.0012, crown=0.012)
    add("Lumbar.L", B, [(0.055, z(0.48)), (0.14 * w, z(0.46)), (0.16 * w, z(0.22)), (0.055, z(0.25))],
        relief=0.0008, crown=0.02)
    line("BladeLine.L", B, [(0.03, z(0.80)), (0.10 * w, z(0.66)), (0.15 * w, z(0.52)), (0.16 * w, z(0.30))], 0.7)
    add("Glute.L", B, [(0.012, z(0.12)), (0.09, z(0.16)), (0.17 * w, z(0.06)), (0.17 * w, z(-0.20)), (0.10, z(-0.30)),
                       (0.012, z(-0.24))], relief=0.0012, crown=0.04, round=0.03)

    # ---- limbs ----
    for t in ("L",):
        sh, el, wr = J[f"UpperArm.{t}"][0], J[f"LowerArm.{t}"][0], J[f"Hand.{t}"][0]
        hp, kn, an = J[f"UpperLeg.{t}"][0], J[f"LowerLeg.{t}"][0], J[f"Foot.{t}"][0]
        ua = Chart("limb", 1.0, sh, el, seam=-150, radius=0.05)
        fa = Chart("limb", 1.0, el, wr, seam=-150, radius=0.04)
        th = Chart("limb", 1.0, hp, kn, seam=-120, radius=0.075)
        sn = Chart("limb", 1.0, kn, an, seam=-120, radius=0.055)
        da, dl = _unit(el - sh), _unit(kn - hp)
        arm = (sh - da * 0.10, el + da * 0.02, 0.13)
        farm = (el - da * 0.01, wr, 0.08)
        leg = (hp + dl * 0.03, kn + dl * 0.02, 0.14)
        shin = (kn + _unit(an - kn) * 0.01, an, 0.11)
        add("Deltoid.L", ua, [(-80, -0.25), (40, -0.32), (150, -0.30), (230, -0.20), (220, 0.36), (130, 0.46), (60, 0.42),
                              (-60, 0.30)], g=arm, relief=0.0014, crown=0.03, round=0.012)
        add("Bicep.L", ua, [(-70, 0.42), (20, 0.48), (55, 0.55), (55, 0.92), (-70, 0.92)], g=arm, relief=0.0012, crown=0.02)
        add("Tricep.L", ua, [(120, 0.50), (215, 0.40), (225, 0.88), (120, 0.92)], g=arm, relief=0.0010, crown=0.02)
        add("Forearm.L", fa, [(-60, 0.08), (100, 0.06), (110, 0.55), (60, 0.90), (-70, 0.90), (-80, 0.50)], g=farm,
            relief=0.0010, crown=0.02)
        add("Quad.L", th, [(-55, 0.10), (40, 0.04), (75, 0.20), (70, 0.70), (30, 0.86), (-20, 0.88), (-60, 0.70)], g=leg,
            relief=0.0014, crown=0.04, round=0.014)
        add("Vastus.L", th, [(-100, 0.50), (-60, 0.55), (-45, 0.88), (-100, 0.92)], g=leg, relief=0.0012, crown=0.02)
        add("ITBand.L", th, [(85, 0.12), (105, 0.12), (105, 0.86), (85, 0.86)], g=leg, relief=0.0004, crown=0.006)
        add("Hamstring.L", th, [(140, 0.18), (235, 0.20), (235, 0.84), (150, 0.86)], g=leg, relief=0.0010, crown=0.025)
        add("Calf.L", sn, [(130, 0.12), (190, 0.10), (240, 0.14), (230, 0.48), (185, 0.66), (140, 0.48)], g=shin,
            relief=0.0016, crown=0.03, round=0.012)
        add("Shin.L", sn, [(-60, 0.10), (60, 0.10), (65, 0.80), (-60, 0.80)], g=shin, relief=0.0006, crown=0.015)
    return out


def flank(P, torso: Torso, J, key="orion"):
    """Distance (metres) to the side seam of the torso: the line where its front meets its back,
    from the armpit to the hip, and a 0/1 mask of where it applies."""
    hips, neck = J["Hips"][0], J["Neck"][0]
    yc = torso.centre(P[:, 2])
    top = neck[2] - 0.13 if key != "nova" else hips[2] + 0.66 * (neck[2] - hips[2])  # under the bust
    on = (np.abs(P[:, 0]) > 0.09) & (P[:, 2] > hips[2] - 0.08) & (P[:, 2] < top)
    return np.abs(P[:, 1] - yc), on
