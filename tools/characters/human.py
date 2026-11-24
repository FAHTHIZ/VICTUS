# SPDX-License-Identifier: Apache-2.0
"""The unhelmeted look of the Frame characters: a real human head under the helmet
(docs/CHARACTERS.md#head-variants, ADR-0247).

The ORION & NOVA design sheet shows Nova's face and her black bob, and Orion behind a closed
helmet; both characters can now be shown either way. Every part below is an extra glTF primitive
of the same <Name>_Body mesh, so it shares the skeleton, the blend shapes and the clips; the
engine shows one set or the other (project::set_character_helmet):

  Skin  the MakeHuman head and neck (unsmoothed: real nose, lips, ears), from just under the
        collar shell up; it carries the MakeHuman facial units under the face-display names
        (eyeBlink_L, mouthSmile_R, ...) so the same face clips drive the glyphs and the face;
  Eye   the high-poly eye proxy plus the teeth and tongue (lower teeth and tongue ride the Jaw);
  Hair  one alpha-tested, double-sided atlas: the scalp shell, the brows and the lashes
        (assemble.py's groom, reused) and, for Nova, a card-based bob with full bangs.

The bob is generated, not sculpted: strands fall along great circles from a crown point over the
convex hull of the head (an envelope a few millimetres to two centimetres off the scalp, outer
layers further out), hang straight down from the widest point of the skull, sweep behind the ears
and are cut to the bob line, curling under at the tips. Each strand becomes a ribbon card bent
across its width; cards sample one of eight strand tiles in the top half of the atlas (UV u runs
along the strand, so the anisotropic specular follows the hair), the unique groom pieces share the
bottom half.
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np

import assemble
import fit
from assemble import Part
from meshops import Quads, repack_islands, vertex_normals, triangulate


@dataclass
class HeadStyle:
    skin: tuple
    lips: tuple
    iris: tuple
    hair: tuple
    hair_style: str  # "crop" | "bob"
    stubble: float
    freckles: float
    brow_thickness: float
    hair_rough: float = 0.55
    hair_anisotropy: float = 0.0  # KHR_materials_anisotropy strength on the Hair material
    lip_size: float = 1.0  # painted lip outline scale (materials.skin_maps)
    makeup: float = 0.0  # lash-line liner, soft eyeshadow, blush and lip gradient (ADR-0252)
    glow: tuple = (0.0, 0.0, 0.0)
    # assemble.Style fields the reused groom builder reads.
    suit: tuple = (0.0, 0.0, 0.0)
    trim: tuple = (0.0, 0.0, 0.0)


HEAD_STYLES = {
    # Orion: the sheet never opens his helmet; a short dark crop and a trimmed beard shadow.
    "orion": HeadStyle(skin=(0.235, 0.120, 0.074), lips=(0.20, 0.085, 0.07), iris=(0.09, 0.048, 0.020),
                       hair=(0.010, 0.008, 0.007), hair_style="crop", stubble=0.8, freckles=0.0, brow_thickness=1.1,
                       hair_rough=0.62),
    # Nova (the sheet's head close-up): fair East Asian skin, dark brown eyes, glossy black bob.
    # (ADR-0252: paler porcelain skin and rosier lips, after the close-up.)
    "nova": HeadStyle(skin=(0.46, 0.315, 0.245), lips=(0.38, 0.15, 0.14), iris=(0.040, 0.022, 0.012),
                      hair=(0.0085, 0.0085, 0.0100), hair_style="bob", stubble=0.0, freckles=0.0, brow_thickness=0.90,
                      hair_rough=0.40, hair_anisotropy=0.88, lip_size=0.80, makeup=1.0),
}

# Card tiles: the top CARD_REGION of the Hair atlas holds TILES strand tiles stacked in v.
TILES = 8
CARD_REGION = 0.5


def _unit(v):
    v = np.asarray(v, float)
    return v / np.linalg.norm(v, axis=-1, keepdims=True)


def _smooth(t):
    t = np.clip(t, 0, 1)
    return t * t * (3 - 2 * t)


def _band(x, x0, x1, x2, x3):
    """1 between x1 and x2, easing to 0 at x0 and x3."""
    return _smooth((x - x0) / (x1 - x0)) * (1.0 - _smooth((x - x2) / (x3 - x2)))


def collar_top(J):
    """Height of the collar shell's top edge (the skin starts under it)."""
    neck0, head0 = J["Neck"][0], J["Head"][0]
    return neck0[2] + 0.55 * (head0[2] - neck0[2])


# ---- skin -----------------------------------------------------------------------------------


def skin_part(body: fit.Body, face_shapes):
    obj = body.obj
    faces = obj.group_faces("body")
    q_all = assemble.obj_quads(obj, faces, body.v)
    J = assemble.joint_map(body)
    c = q_all.v[q_all.f].mean(1)
    z0 = collar_top(J) - 0.03
    sel = np.where((c[:, 2] > z0) & (np.abs(c[:, 0]) < 0.11))[0]
    q, used = q_all.subset(sel)
    q = repack_islands(q)
    morphs = {k: d[used] for k, d in body.morphs.items() if k in face_shapes}
    return Part("Skin", "Skin", q, body.weights[used], morphs, attrs={"rest": q.v.copy()}), q, used


class NovaSculpt:
    """The sheet's Nova face from the MakeHuman base: thinner, smaller lips, slightly larger eyes,
    a narrower V jaw and a shorter lower face. Smooth displacement fields (applied to the skin,
    the eyes/mouth and the lashes alike so they stay together) that vanish above the brows and
    well above the neck seam."""

    def __init__(self, skin: Part, mouth: Part, J):
        v = skin.q.v
        self.mz = float(np.median(mouth.q.v[:, 2]))
        near = (np.abs(v[:, 0]) < 0.03) & (np.abs(v[:, 2] - self.mz) < 0.02)
        self.my = float(v[near, 1].min())  # front of the lips
        self.eyes = [J["Eye.L"][0], J["Eye.R"][0]]
        self.eye_z = 0.5 * (self.eyes[0][2] + self.eyes[1][2])
        self.nose_z = self.mz + 0.34 * (self.eye_z - self.mz)  # base of the nose
        self.chin_z = float(v[(np.abs(v[:, 0]) < 0.01) & (v[:, 1] < self.my + 0.03), 2].min())
        self.nose_y = float(v[(np.abs(v[:, 0]) < 0.01) & (np.abs(v[:, 2] - self.nose_z - 0.01) < 0.01), 1].min())

    def eyes_only(self, v):
        d = v.copy()
        for e in self.eyes:
            r = np.linalg.norm(v - e, axis=1)
            w = _smooth((0.032 - r) / 0.018)
            d += (v - e) * (0.17 * w)[:, None]
        return d

    def __call__(self, v):
        mz, my = self.mz, self.my
        d = self.eyes_only(v)
        front = _smooth((my + 0.10 - v[:, 1]) / 0.06)
        # Lips: squeeze toward the lip line, narrow toward the centre, ease the pout back.
        dx, dz, dy = v[:, 0] / 0.030, (v[:, 2] - mz) / 0.016, (v[:, 1] - my) / 0.022
        w = np.exp(-(dx ** 2 + dz ** 2 + dy ** 2) * 1.5)
        # (ADR-0252: the sheet's lips are soft and full, so the squeeze is gentle.)
        d[:, 2] += (v[:, 2] - mz) * (-0.04 * w)
        d[:, 0] += v[:, 0] * (-0.14 * w)
        d[:, 1] -= 0.0018 * w * np.clip(1 - dy, 0, 1)
        # Shorter lower face: lift everything between the nose base and the chin (and the
        # underside of the jaw with it), 9 % of the nose-to-chin height at the chin.
        span = self.nose_z - self.chin_z
        t = np.clip((self.nose_z - v[:, 2]) / span, 0, 1)
        under = _smooth((v[:, 2] - (self.chin_z - 0.05)) / 0.05)
        lift = 0.09 * span * t * np.where(v[:, 2] < self.chin_z, under, 1.0)
        d[:, 2] += lift * front
        # Jaw: narrow the lower face toward the chin (a soft V).
        below = _smooth((mz + 0.012 - v[:, 2]) / 0.05) * (v[:, 2] > self.chin_z - 0.06)
        d[:, 0] -= v[:, 0] * (0.09 * below * front)
        # ADR-0254 (the sheet's close-up): slimmer lower cheeks under a defined cheekbone, the
        # outer eye corners lifted a little, and a small forward chin point.
        ax_ = np.abs(v[:, 0])
        cheek = (_smooth((ax_ - 0.020) / 0.020) * np.exp(-((v[:, 2] - (mz + 0.020)) / 0.022) ** 2) * front)
        d[:, 0] -= np.sign(v[:, 0]) * 0.0035 * cheek
        bone = _smooth((ax_ - 0.030) / 0.015) * np.exp(-((v[:, 2] - (self.eye_z - 0.022)) / 0.010) ** 2) * front
        d[:, 0] += np.sign(v[:, 0]) * 0.0012 * bone
        for e in self.eyes:
            oc = np.exp(-(((v[:, 0] - e[0] * 1.45) / 0.010) ** 2 + ((v[:, 2] - e[2]) / 0.010) ** 2)) * front
            d[:, 2] += 0.0016 * oc
        chin = np.exp(-((v[:, 0] / 0.012) ** 2 + ((v[:, 2] - self.chin_z - 0.008) / 0.012) ** 2)) * front
        d[:, 1] -= 0.0018 * chin
        # Nose (ADR-0252): a higher, finer bridge between the eyes and down the dorsum, and a
        # slightly narrower tip.
        ny = self.nose_y
        bridge = np.exp(-(v[:, 0] / 0.009) ** 2) * _band(v[:, 2], self.nose_z + 0.004, self.nose_z + 0.016,
                                                         self.eye_z + 0.002, self.eye_z + 0.016)
        d[:, 1] -= 0.0022 * bridge * _smooth((ny + 0.03 - v[:, 1]) / 0.02)
        tip = np.exp(-((v[:, 2] - self.nose_z - 0.008) / 0.012) ** 2) * _smooth((ny + 0.022 - v[:, 1]) / 0.012)
        d[:, 0] -= v[:, 0] * (0.10 * tip * (np.abs(v[:, 0]) < 0.03))
        return d

    def lids(self, v):
        """Opens the eyes (skin and lashes only; the eyeballs keep their shape): the lid aperture
        stretched vertically about each eye's centre line, the upper lid most."""
        d = v.copy()
        for e in self.eyes:
            dz = v[:, 2] - e[2]
            w = (_smooth((0.020 - np.abs(v[:, 0] - e[0])) / 0.008) * _smooth((0.016 - np.abs(dz)) / 0.008)
                 * _smooth((e[1] + 0.004 - v[:, 1]) / 0.008))
            d[:, 2] += dz * np.where(dz > 0, 0.60, 0.30) * w
            d[:, 0] += (v[:, 0] - e[0]) * 0.10 * w
        return d


def sculpt_nova(skin: Part, eye: Part, mouth: Part, J):
    f = NovaSculpt(skin, mouth, J)
    skin.q.v[:] = f.lids(f(skin.q.v))
    skin.attrs["rest"] = skin.q.v.copy()
    eye.q.v[:] = f(eye.q.v)
    eye.attrs["rest"] = eye.q.v.copy()
    mouth.q.v[:] = f(mouth.q.v)
    return f


def eye_part(body: fit.Body, face_shapes):
    """Eyes, teeth and tongue as one glossy part (kind 0 eye, 1 mouth)."""
    e = assemble.eye_part(body)
    m = assemble.mouth_part(body)
    nv = len(e.q.v)
    q = Quads(np.concatenate([e.q.v, m.q.v]), np.concatenate([e.q.f, m.q.f + nv]),
              np.concatenate([e.q.uv, m.q.uv]), np.concatenate([e.q.fuv, m.q.fuv + len(e.q.uv)]))
    kind = np.concatenate([np.zeros(len(e.q.f), np.int32), np.ones(len(m.q.f), np.int32)])
    q = repack_islands(q, density=np.where(kind == 0, 2.0, 1.0))
    W = np.concatenate([e.W, m.W])
    names = set(face_shapes)
    morphs = {}
    for k in names:
        de = e.morphs.get(k)
        dm = m.morphs.get(k)
        if de is None and dm is None:
            continue
        morphs[k] = np.concatenate([de if de is not None else np.zeros((nv, 3)),
                                    dm if dm is not None else np.zeros((len(m.q.v), 3))])
    z = np.zeros((nv, 1))
    zm = np.zeros((len(m.q.v), 1))
    attrs = {"theta": np.concatenate([e.attrs["theta"], zm]), "phi": np.concatenate([e.attrs["phi"], zm]),
             "label": np.concatenate([z, m.attrs["label"]]), "z": np.concatenate([z, m.attrs["z"]]),
             "rest": q.v.copy()}
    return Part("Eye", "Eye", q, W, morphs, attrs=attrs, face_attrs={"kind": kind}), m


# ---- hair cards -----------------------------------------------------------------------------


class Envelope:
    """Convex hull of the (blurred) head, read as a radius toward any direction from its centre."""

    def __init__(self, head):
        import gear

        self.c = head.c
        pts = head.surface(gear.cube_sphere(40).v)
        self.eq = gear._hull(pts, 0.0)

    def radius(self, d):
        import gear

        return gear._ray_exit(self.eq, self.c, _unit(d))


def _az_dir(phi):
    """Horizontal unit direction at azimuth phi (radians): 0 = front (-Y), +pi/2 = the
    character's left (+X)."""
    return np.stack([np.sin(phi), -np.cos(phi), np.zeros_like(phi)], -1)


def bob_strands(env: Envelope, J, chin_z, seed=3):
    """Guide strands of the bob: list of (points (n, 3), layer, width, azimuth)."""
    rng = np.random.default_rng(seed)
    c = env.c
    eye = 0.5 * (J["Eye.L"][0] + J["Eye.R"][0])
    ez = eye[2]
    pole = _unit([0.0, 0.32, 1.0])  # the crown whorl, a little behind the top of the head
    # The ear spans azimuth ~70-100 degrees and rises ~2 cm above the eye line (MakeHuman head).
    ear_top = ez + 0.024
    # Closer to the scalp than a helmet-like bowl (the sheet's bob is soft and sleek on top).
    # ADR-0252: fuller at the crown and the back (the sheet's rounded bob), and denser.
    layers = ((0.0150, 168, 0.005, 0.0115), (0.0110, 132, 0.30, 0.0110), (0.0068, 100, 0.55, 0.0105))
    # Side part on her left (+X): the fringe and the front of the crown radiate from a parting
    # forward and to the left of the whorl, so they sweep across the forehead to her right.
    part_pole = _unit([0.24, -0.06, 1.0])
    out = []

    def cut_z(az):
        sa = np.degrees(az)
        a = np.abs(sa)
        # Bangs to the brows (a little longer toward the temples), face-framing locks to the jaw,
        # the bob to the chin all round, a touch shorter at the nape.
        # (Side-swept: a little longer toward her right.)
        # ADR-0252 (the sheet's close-ups): a long swept fringe, to the lashes at the centre and
        # past the eye on her right (-X), shorter toward the parting; face-framing locks past the
        # jaw; a graduated A-line hem rising toward the nape.
        bang = ez + 0.003 - 0.006 * (a / 50.0) ** 2 + 0.010 * np.clip(sa / 50.0, -1, 1) * (sa > 0) \
            - 0.014 * np.clip(-sa / 50.0, 0, 1)
        lock = chin_z - 0.004
        side = chin_z + 0.010
        back = chin_z + 0.040
        z = np.where(a < 46, bang,
                     np.where(a < 56, bang + (lock - bang) * _smooth((a - 46) / 10.0),
                              np.where(a < 120, lock + (side - lock) * _smooth((a - 56) / 40.0),
                                       side + (back - side) * _smooth((a - 120) / 60.0))))
        return z

    # Piecey hem: low-frequency clumps of shorter and longer pieces around the head (the sheet's
    # choppy cut), gentler on the bangs.
    ks = np.arange(5, 12)
    camp = rng.uniform(0.0012, 0.0026, len(ks))
    cph = rng.random(len(ks)) * 2 * np.pi

    def chop(az):
        return float((camp * np.sin(ks * az + cph)).sum())

    def meridian(az, off, th0, pole=pole):
        w = _az_dir(np.array(az))
        w = _unit(w - pole * (w @ pole))
        th = np.radians(np.linspace(th0, 150.0, 160))
        d = np.cos(th)[:, None] * pole + np.sin(th)[:, None] * w
        r = env.radius(d) + off
        return c + d * r[:, None]

    grid = np.radians(np.linspace(-180, 180, 241))
    wide = []
    for off, *_ in layers:
        row = []
        for g in grid:
            Mg = meridian(g, off, 10.0)
            row.append(np.linalg.norm((Mg - c)[:, :2], axis=1)[Mg[:, 2] > ez - 0.06].max())
        wide.append(np.array(row))

    def widest(li, a):
        return float(np.interp(a, grid, wide[li]))

    for li, (off, count, th_frac, width) in enumerate(layers):
        # Denser over the front half (bangs and face-framing locks read from the front).
        u = (np.arange(count) + 0.5) / count
        az = np.pi * (2 * u - 1)
        az = az + rng.normal(0, 0.35 / count * 2 * np.pi, count)
        az = np.sign(az) * np.pi * (np.abs(az) / np.pi) ** 1.12
        for a in az:
            a_deg = abs(np.degrees(a))
            th0 = 4.0 + 6.0 * rng.random() + th_frac * (40.0 if a_deg > 50 else 22.0)
            # Front strands start from the parting (blended into the whorl toward the sides).
            pb = _smooth((75.0 - a_deg) / 30.0)
            pl = _unit(pole * (1 - pb) + part_pole * pb)
            # Volume: the outer layer stands further off over the crown and the back.
            lift = 0.004 * _smooth((a_deg - 60.0) / 60.0) * (li == 0)
            M = meridian(a, off + lift + rng.normal(0, 0.0012), th0 * (1 - 0.6 * pb), pl)
            rel = M - c
            rho = np.linalg.norm(rel[:, :2], axis=1)
            zc = cut_z(np.array(a)) + rng.normal(0, 0.0055 if a_deg < 50 else 0.0035) + chop(a) * (0.8 if a_deg < 50 else 1.0)
            if a_deg < 46:
                # Wispy fringe: some pieces cut short (forehead shows through), a few left long.
                r_ = rng.random()
                zc += rng.uniform(0.007, 0.014) if r_ < 0.22 else (-rng.uniform(0.003, 0.007) if r_ > 0.85 else 0.0)
            above = M[:, 2] > ez - 0.06
            k = int(np.argmax(np.where(above, rho, -1)))
            pts = list(M[: k + 1])
            if M[k, 2] > zc:
                # Hang straight down from the widest point, sweeping behind the ear on the sides.
                zs = np.linspace(M[k, 2], zc, max(4, int((M[k, 2] - zc) / 0.004)))[1:]
                for z in zs:
                    s = _smooth((ear_top + 0.045 - z) / 0.045)
                    ae = abs(a)
                    # Face-framing pieces fall in front of the ear; behind them the hair tucks.
                    # ADR-0254 (the sheet's three-quarter view): the hair is tucked behind the ear,
                    # which shows; only the locks in front of it frame the cheek.
                    aa = np.sign(a) * ae
                    # Taper in toward the chin: the bob hugs the jaw rather than flaring.
                    fall = (M[k, 2] - z) / max(M[k, 2] - zc, 1e-6)
                    rr = (rho[k] + (widest(li, aa) - rho[k]) * s) * (1 - 0.03 * fall ** 1.5)
                    p = np.array([c[0], c[1], 0.0]) + _az_dir(np.array(aa)) * rr
                    p[2] = z
                    pts.append(p)
            pts = np.array(pts)
            # ADR-0254 (the sheet's three-quarter view): around the ear the hair is tucked behind
            # it, so the ear shows; only the locks in front of it frame the cheek. Applied to the
            # whole path (the skull's widest point is at ear height, so the crown meridians reach
            # it too), rotating each point about the head axis.
            ae_ = abs(a)
            if np.radians(56) < ae_ < np.radians(132):
                tgt = min(ae_, np.radians(62)) if ae_ < np.radians(66) else \
                    np.radians(100) + (ae_ - np.radians(66)) / np.radians(66) * np.radians(32)
                rel_ = pts - c
                s_ = _smooth((ear_top + 0.012 - pts[:, 2]) / 0.024)
                ang = np.sign(a) * (tgt - ae_) * s_  # rotation about the vertical head axis
                ca, sa_ = np.cos(ang), np.sin(ang)
                # Azimuth phi = atan2(x, -y); rotating by +ang maps (x, y) as below.
                x_ = rel_[:, 0] * ca - rel_[:, 1] * sa_
                y_ = rel_[:, 0] * sa_ + rel_[:, 1] * ca
                pts = np.stack([c[0] + x_, c[1] + y_, pts[:, 2]], 1)
            pts = pts[pts[:, 2] >= zc - 1e-6] if (pts[:, 2] < zc).any() else pts
            if len(pts) < 4:
                continue
            # Resample by arc length, then curl the last few centimetres under (toward the head
            # axis); bangs curl gently toward the forehead.
            seg = np.linalg.norm(np.diff(pts, axis=0), axis=1)
            sarc = np.concatenate([[0], np.cumsum(seg)])
            L = sarc[-1]
            n = 14 if a_deg > 50 else 10
            ts = np.linspace(0, L, n + 1)
            P = np.stack([np.interp(ts, sarc, pts[:, i]) for i in range(3)], 1)
            t = ts / L
            curl_len = 0.035 if a_deg > 50 else 0.02
            amt = (0.011 if a_deg > 50 else 0.004) * (1.0 - 0.3 * li)
            k_ = _smooth((ts - (L - curl_len)) / curl_len) ** 1.5
            radial = P - np.array([c[0], c[1], 0.0])
            radial[:, 2] = 0
            radial = _unit(radial + 1e-9)
            P = P - radial * (k_ * amt)[:, None]
            # Loose tips: each strand drifts a few millimetres sideways toward its end, so the
            # hair does not read as combed into one shell.
            tang = _az_dir(np.array(a) + np.pi / 2)
            P = P + tang[None] * (rng.normal(0, 0.0026) * t ** 2)[:, None]
            if a_deg < 60:
                # The fringe sweeps toward her right (-X), most at the centre and on the parting
                # side, and its tips lift a little off the brow.
                sweep = 0.024 * (1 - (a_deg / 60.0) ** 2) * (0.75 if a > 0 else 1.0)
                P = P + np.array([-1.0, 0.0, 0.0])[None] * (sweep * t ** 1.6)[:, None]
                P = P + np.array([0.0, -1.0, 0.0])[None] * (0.002 * t ** 2)[:, None]
            # Narrower fringe pieces, so gaps show between them (the sheet's wispy bangs).
            wf = 0.78 if a_deg < 46 and li == 0 else 1.0
            out.append((P, li, width * wf * (0.9 + 0.25 * rng.random()), a, t))
    return out


def strand_cards(strands, env: Envelope, seed=5):
    """Ribbon cards along the guide strands: Quads, per-vertex card coordinates (s across,
    t along), per-vertex layer, and the card tile per face."""
    rng = np.random.default_rng(seed)
    vs, fs, uvs, fuvs, S, T, LAY, tile_f = [], [], [], [], [], [], [], []
    nv = 0
    c = env.c
    across = 3
    for P, li, width, az, t in strands:
        n = len(P)
        tan = np.gradient(P, axis=0)
        tan = _unit(tan)
        rad = P - c
        rad = _unit(rad - tan * np.sum(rad * tan, 1, keepdims=True))
        b = _unit(np.cross(tan, rad))
        twist = rng.normal(0, 0.18)
        b = _unit(b * np.cos(twist) + rad * np.sin(twist))
        nrm = _unit(np.cross(b, tan))
        if np.sum(nrm * rad) < 0:
            nrm = -nrm
        # Narrow at the crown, where every strand converges.
        w = width * (0.35 + 0.65 * _smooth(t / 0.3))
        # Piecey, pointed ends (ADR-0252): every card tapers over its last fifth.
        w = w * (1.0 - 0.65 * _smooth((t - 0.78) / 0.22))
        rows = []
        for j in range(across):
            s = j / (across - 1) - 0.5
            bulge = (1 - (2 * s) ** 2) * 0.0012
            rows.append(P + b * (s * w)[:, None] + nrm * bulge)
        V = np.stack(rows, 1).reshape(-1, 3)  # (n * across)
        idx = np.arange(n * across).reshape(n, across)
        f = np.stack([idx[:-1, :-1], idx[1:, :-1], idx[1:, 1:], idx[:-1, 1:]], -1).reshape(-1, 4)
        # Outer-layer cards use the bright tiles, inner layers the darker ones (shared tiles cannot
        # carry per-card shading: the cards overlap in UV).
        tile = int(rng.integers(TILES // 2)) + (TILES // 2 if li > 0 else 0)
        flip = rng.random() < 0.5
        sv = np.tile(np.linspace(0, 1, across), n)
        if flip:
            sv = 1 - sv
        tv = np.repeat(t, across)
        # UV: u along the strand (the tangent the anisotropic lobe follows), v across inside the tile.
        uv = np.stack([0.002 + tv * 0.996, (tile + 0.06 + 0.88 * sv) * (CARD_REGION / TILES)], 1)
        vs.append(V)
        fs.append(f + nv)
        uvs.append(uv)
        fuvs.append(f + nv)
        S.append(sv)
        T.append(tv)
        LAY.append(np.full(len(V), li))
        tile_f.append(np.full(len(f), tile))
        nv += len(V)
    q = Quads(np.concatenate(vs), np.concatenate(fs), np.concatenate(uvs), np.concatenate(fuvs))
    # Faces wound outward (away from the head centre).
    nrm = vertex_normals(q.v, triangulate(q.f))
    fc = q.v[q.f].mean(1)
    fn = nrm[q.f].mean(1)
    flipf = ((fc - c) * fn).sum(1) < 0
    q.f[flipf] = q.f[flipf][:, ::-1]
    q.fuv[flipf] = q.fuv[flipf][:, ::-1]
    return q, np.concatenate(S), np.concatenate(T), np.concatenate(LAY), np.concatenate(tile_f)


# ---- hair part ------------------------------------------------------------------------------


def hair_part(body: fit.Body, hs: HeadStyle, head, face_shapes, chin_z):
    obj = body.obj
    faces = obj.group_faces("body")
    q_full = assemble.obj_quads(obj, faces, body.v)
    q_body, used_body = q_full.subset(np.arange(len(faces)))
    nrm = vertex_normals(q_body.v, triangulate(q_body.f))
    gstyle = assemble.Style(skin=hs.skin, lips=hs.lips, iris=hs.iris, hair=hs.hair, suit=hs.suit, trim=hs.trim,
                            glow=hs.glow, hair_style=hs.hair_style, stubble=hs.stubble, freckles=hs.freckles,
                            brow_thickness=hs.brow_thickness)
    pieces = assemble.groom_parts(body, gstyle, q_body, nrm, used_body)
    pieces += assemble.lash_part(body)
    names = [n for n, *_ in body.joints]
    keys = ("flow", "margin", "density", "s", "t", "upper", "kind", "layer")
    width = {"flow": 3}
    pos, f, uv, fuv, W, attrs, morphs_l, dens = [], [], [], [], [], {}, [], []
    nv = nuv = 0
    for name, q, p, used, a in pieces:
        pos.append(p)
        f.append(q.f + nv)
        fuv.append(q.fuv + nuv)
        uv.append(q.uv)
        W.append(body.weights[used])
        morphs_l.append((used, len(p)))
        dens.append(np.full(len(q.f), {"Hair": 1.0, "Brow": 2.5, "Lash": 6.0}[name]))
        for k in keys:
            attrs.setdefault(k, []).append(a.get(k, np.zeros((len(p), width.get(k, 1)))))
        nv += len(p)
        nuv += len(q.uv)
    gq = Quads(np.concatenate(pos), np.concatenate(f), np.concatenate(uv), np.concatenate(fuv))
    # The unique pieces own the bottom of the atlas: pack into a (1 - CARD_REGION)-tall strip.
    region = 1.0 - CARD_REGION
    gq = repack_islands(gq, density=np.concatenate(dens), padding=0.004, aspect=region)
    gq = Quads(gq.v, gq.f, gq.uv + np.array([0.0, CARD_REGION]), gq.fuv)
    gW = np.concatenate(W)
    gmorph = {}
    off = 0
    for used, n in morphs_l:
        for k, d in body.morphs.items():
            if k in face_shapes:
                gmorph.setdefault(k, []).append(d[used])
        off += n
    gmorph = {k: np.concatenate(v) for k, v in gmorph.items() if len(v) == len(morphs_l)}
    gattrs = {k: np.concatenate(v) for k, v in attrs.items()}
    gkind = gattrs["kind"][:, 0]
    tile_face = np.full(len(gq.f), -1)

    if hs.hair_style == "bob":
        env = Envelope(head)
        strands = bob_strands(env, assemble.joint_map(body), chin_z)
        cq, S, T, LAY, tile_f = strand_cards(strands, env)
        cW = np.zeros((len(cq.v), len(names)))
        cW[:, names.index("Head")] = 1.0
        nvg = len(gq.v)
        gq = Quads(np.concatenate([gq.v, cq.v]), np.concatenate([gq.f, cq.f + nvg]),
                   np.concatenate([gq.uv, cq.uv]), np.concatenate([gq.fuv, cq.fuv + len(gq.uv)]))
        gW = np.concatenate([gW, cW])
        gmorph = {k: np.concatenate([d, np.zeros((len(cq.v), 3))]) for k, d in gmorph.items()}
        extra = {"flow": np.zeros((len(cq.v), 3)), "margin": np.full((len(cq.v), 1), 0.05),
                 "density": np.zeros((len(cq.v), 1)), "s": S[:, None], "t": T[:, None],
                 "upper": np.zeros((len(cq.v), 1)), "kind": np.full((len(cq.v), 1), 3.0), "layer": LAY[:, None].astype(float)}
        gattrs = {k: np.concatenate([gattrs[k], extra[k]]) for k in keys}
        tile_face = np.concatenate([tile_face, tile_f])
    gattrs["rest"] = gq.v.copy()
    del gkind
    face_kind = np.rint(gattrs["kind"][gq.f[:, 0], 0]).astype(np.int32)
    return Part("Hair", "Hair", gq, gW, gmorph, attrs=gattrs, face_attrs={"kind": face_kind, "tile": tile_face.astype(np.int32)})


def build(body: fit.Body, head, face_shapes):
    """The unhelmeted head parts: Skin, Eye, Hair; and the landmarks their bakes need."""
    import materials

    hs = HEAD_STYLES[body.spec.key]
    skin, skin_q, _ = skin_part(body, face_shapes)
    eye, mouth = eye_part(body, face_shapes)
    sculpt = sculpt_nova(skin, eye, mouth, assemble.joint_map(body)) if body.spec.key == "nova" else None
    L = materials.Landmarks(body, [skin, mouth])
    hair = hair_part(body, hs, head, face_shapes, L.chin[2])
    if sculpt is not None:  # brows and lashes follow the eyes
        k = np.rint(hair.attrs["kind"][:, 0])
        m = (k == 1) | (k == 2)
        hair.q.v[m] = sculpt.lids(sculpt.eyes_only(hair.q.v[m]))
        if "rest" in hair.attrs:
            hair.attrs["rest"] = hair.q.v.copy()
    return [skin, eye, hair], hs, L
