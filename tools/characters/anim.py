# SPDX-License-Identifier: Apache-2.0
"""Animation library of the VICTUS starter characters (docs/CHARACTERS.md, docs/ANIMATION.md, ADR-0179).

Clips are procedural keyframe samplers: each returns, for a phase u in [0, 1], per-joint rotations
expressed in authoring (armature) axes, the Hips offset, and blend-shape weights. build_characters
converts rotations into each joint's rest frame (rotation-only clips, so one library fits every
VICTUS Humanoid profile and retargets by joint name). Conventions on a joint hanging from its
parent: rx(+) swings a limb backward, roll_out(+) lifts it sideways. Arm angles are measured from
the arm hanging straight down (Pose.arm_rest removes the rest A-pose's sideways angle).

The original seven clips keep their names and durations (Idle 4.0, Walk 0.8, Run 0.55, Jump 0.6,
Fall 0.8, Land 0.4, Interact 0.9 s) so existing controllers and scripts keep working.
"""
from __future__ import annotations

import math

import numpy as np

FPS = 30


def q_axis(axis, deg):
    a = np.asarray(axis, float)
    a = a / np.linalg.norm(a)
    h = math.radians(deg) * 0.5
    return np.array([*(a * math.sin(h)), math.cos(h)])


def q_mul(a, b):
    x1, y1, z1, w1 = a
    x2, y2, z2, w2 = b
    return np.array([w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2, w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2,
                     w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2, w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2])


def q_conj(q):
    return np.array([-q[0], -q[1], -q[2], q[3]])


def chain(*qs):
    out = np.array([0, 0, 0, 1.0])
    for q in qs:
        out = q_mul(out, q)
    return out


def rx(d):
    return q_axis((1, 0, 0), d)


def yaw(d):
    return q_axis((0, 0, 1), d)


def roll_y(d):
    return q_axis((0, 1, 0), d)


def roll_out(side, d):
    return q_axis((0, 1, 0), -side * d)


def smooth(t):
    t = min(max(t, 0.0), 1.0)
    return t * t * (3 - 2 * t)


def ramp(u, a, b):
    return smooth((u - a) / (b - a))


def bump(u, a, b, c, d):
    return ramp(u, a, b) * (1 - ramp(u, c, d))


class Pose:
    def __init__(self, hand_axes, arm_rest=None):
        self.q = {}
        self.loc = np.zeros(3)
        self.morph = {}
        self.hand_axes = hand_axes  # joint -> curl axis (authoring space)
        # Sideways angle (degrees) of each upper arm from hanging straight down in the rest
        # A-pose, by side (+1 left, -1 right). arm() angles are measured from hanging down.
        self.arm_rest = arm_rest or {1: 0.0, -1: 0.0}

    def rot(self, joint, *qs):
        self.q[joint] = chain(self.q.get(joint, np.array([0, 0, 0, 1.0])), *qs)
        return self

    def arm(self, side, pitch, out=0.0, elbow=-15.0, hand=-6.0, twist=0.0, wrist_side=0.0):
        """pitch swings forward (-) / back (+), out lifts sideways from hanging straight down,
        elbow and hand bend about the hanging arm's lateral axis."""
        t = "L" if side > 0 else "R"
        down = roll_out(side, -self.arm_rest[side])  # rest A-pose -> hanging
        up = q_conj(down)
        self.rot(f"UpperArm.{t}", rx(pitch), roll_out(side, out), yaw(side * twist), down)
        # Children keep their rest frames: express their bends in the hanging arm's axes.
        self.rot(f"LowerArm.{t}", up, rx(elbow), down)
        self.rot(f"Hand.{t}", up, rx(hand), roll_out(side, wrist_side), down)

    def leg(self, side, thigh, knee, foot_net, toe=0.0, spread=0.0, turn=0.0):
        t = "L" if side > 0 else "R"
        self.rot(f"UpperLeg.{t}", rx(thigh), roll_out(side, spread), yaw(turn))
        self.rot(f"LowerLeg.{t}", rx(knee))
        self.rot(f"Foot.{t}", rx(foot_net - thigh - knee), roll_out(side, -spread))
        self.rot(f"Toe.{t}", rx(toe))

    def fingers(self, side, curl, thumb=None, spread=0.0, per=(1.0, 1.0, 1.0, 1.0)):
        """curl in degrees per phalanx (proximal joints a bit less than the distal ones)."""
        t = "L" if side > 0 else "R"
        for i, f in enumerate(("Index", "Middle", "Ring", "Pinky")):
            c = curl * per[i]
            for j, k in ((1, 0.9), (2, 1.1), (3, 0.8)):
                n = f"{f}{j}.{t}"
                self.rot(n, q_axis(self.hand_axes[n], c * k))
        th = curl * 0.5 if thumb is None else thumb
        for j, k in ((1, 0.4), (2, 0.8), (3, 0.9)):
            n = f"Thumb{j}.{t}"
            self.rot(n, q_axis(self.hand_axes[n], th * k))

    def face(self, **w):
        for k, v in w.items():
            self.morph[k] = max(self.morph.get(k, 0.0), v)


# ---- leg helpers ------------------------------------------------------------------------


class Body:
    """Lengths of the fitted skeleton the samplers need (thigh, shin)."""

    def __init__(self, thigh=0.42, shin=0.42):
        self.thigh, self.shin = thigh, shin

    def drop(self, thigh, knee):
        a, b = math.radians(thigh), math.radians(thigh + knee)
        return self.thigh * (1 - math.cos(a)) + self.shin * (1 - math.cos(b))


def foot_walk(u):
    if u < 0.08:
        return -8 * (1 - smooth(u / 0.08))
    if u < 0.33:
        return 0.0
    if u < 0.5:
        return 25 * smooth((u - 0.33) / 0.17)
    if u < 0.65:
        return 25 * (1 - smooth((u - 0.5) / 0.15))
    return -8 * smooth((u - 0.65) / 0.35)


# ---- samplers ------------------------------------------------------------------------------


def _relaxed(p: Pose, breath=0.0, curl=14.0):
    p.arm(1, 3 + 1.2 * breath, 9, elbow=-12, hand=-5)
    p.arm(-1, 3 + 1.2 * breath, 9, elbow=-12, hand=-5)
    p.fingers(1, curl, per=(0.8, 1.0, 1.15, 1.3))
    p.fingers(-1, curl, per=(0.8, 1.0, 1.15, 1.3))


def _blink(p: Pose, u, at, dur=0.045):
    b = bump(u, at, at + dur * 0.4, at + dur * 0.6, at + dur)
    if b > 0:
        p.face(eyeBlink_L=b, eyeBlink_R=b)


def idle(b: Body, p: Pose, u):
    ph = 2 * math.pi * u
    breath = math.sin(2 * ph)
    glance = bump(u, 0.52, 0.62, 0.80, 0.92)
    p.rot("Hips", yaw(1.5 * math.sin(ph)))
    p.rot("Spine", rx(1.0))
    p.rot("Chest", rx(-1.2 * breath))
    p.rot("Neck", rx(1.0 + 0.6 * breath))
    p.rot("Head", yaw(16 * glance), rx(2 + 3 * glance))
    p.rot("Eye.L", yaw(10 * glance))
    p.rot("Eye.R", yaw(10 * glance))
    p.rot("Shoulder.L", roll_out(1, 1.5 * breath))
    p.rot("Shoulder.R", roll_out(-1, 1.5 * breath))
    _relaxed(p, breath)
    p.leg(1, -4, 8, 0)
    p.leg(-1, -4, 8, 0)
    p.loc = np.array([0.006 * math.sin(ph), 0, -b.drop(-4, 8) - 0.004 * (1 + breath) / 2])
    _blink(p, u, 0.18)
    _blink(p, u, 0.71)


def idle_look(b, p, u):
    idle(b, p, 0.0)
    p.q.clear()
    ph = 2 * math.pi * u
    breath = math.sin(2 * ph)
    left = bump(u, 0.10, 0.22, 0.40, 0.50)
    right = bump(u, 0.55, 0.66, 0.82, 0.93)
    p.rot("Spine", rx(1.0), yaw(4 * (left - right)))
    p.rot("Chest", rx(-1.2 * breath), yaw(8 * (left - right)))
    p.rot("Head", yaw(28 * (left - right)), rx(3))
    for e in ("Eye.L", "Eye.R"):
        p.rot(e, yaw(14 * (left - right)))
    _relaxed(p, breath)
    p.leg(1, -4, 8, 0)
    p.leg(-1, -4, 8, 0)
    _blink(p, u, 0.24)
    _blink(p, u, 0.69)


def idle_shift(b, p, u):
    """Weight shift from one leg to the other and back (variety idle)."""
    ph = 2 * math.pi * u
    w = math.sin(ph)
    breath = math.sin(3 * ph)
    p.rot("Hips", roll_y(3.5 * w))
    p.rot("Spine", roll_y(-2.5 * w), rx(1))
    p.rot("Chest", roll_y(-1.5 * w), rx(-1.0 * breath))
    p.rot("Head", roll_y(-1.0 * w))
    _relaxed(p, breath)
    p.leg(1, -4 + 3 * max(w, 0), 8 + 10 * max(-w, 0), 0, spread=-3.5 * w)
    p.leg(-1, -4 + 3 * max(-w, 0), 8 + 10 * max(w, 0), 0, spread=3.5 * w)
    p.loc = np.array([0.03 * w, 0, -b.drop(-4, 12)])
    _blink(p, u, 0.4)


GAITS = {
    # stride swing, knee lift, lean, arm swing, elbow, bob, base drop, hips yaw, chest yaw, base knee
    "walk": (28.0, 40.0, 2.0, 24.0, -20.0, 0.025, 0.035, -6, 6, 6),
    "run": (45.0, 75.0, 8.0, 45.0, -80.0, 0.05, 0.06, -9, 12, 15),
    "sprint": (58.0, 95.0, 14.0, 60.0, -90.0, 0.06, 0.07, -11, 15, 18),
    "crouch": (22.0, 30.0, 18.0, 10.0, -35.0, 0.012, 0.0, -4, 4, 70),
}


def gait(b, p, u, kind="walk", direction="fwd"):
    sw, kn, lean, arm_sw, elbow, bob, base, hy, cy, k0 = GAITS[kind]
    run = kind in ("run", "sprint")
    crouch = kind == "crouch"
    back = direction == "back"
    lateral = {"left": 1.0, "right": -1.0}.get(direction, 0.0)
    if back:
        u = (1.0 - u) % 1.0
    ph = 2 * math.pi * u
    c = math.cos(ph)
    fwd_scale = 0.35 if lateral else 1.0
    p.rot("Hips", yaw(hy * c * fwd_scale + 22 * lateral))
    p.rot("Spine", rx(lean * (0.4 if back else 1.0)), yaw((cy * 0.5) * c * fwd_scale - 12 * lateral))
    p.rot("Chest", rx((6 if run else 2) + (10 if crouch else 0)), yaw(cy * c * fwd_scale - 10 * lateral))
    p.rot("Neck", rx(-4 if run else (-8 if crouch else 0)))
    p.rot("Head", rx(-8 if run else (-10 if crouch else -2)))
    thigh0 = -10 if run else (-55 if crouch else 0)
    for side, off in ((1, 0.0), (-1, 0.5)):
        uu = (u + off) % 1.0
        pp = 2 * math.pi * uu
        thigh = thigh0 - sw * math.cos(pp) * fwd_scale
        knee = k0 + kn * max(0.0, -math.sin(pp)) * (0.7 if lateral else 1.0)
        net = foot_walk(uu) * (1.3 if run else 1.0) * fwd_scale
        spread = lateral * side * 9.0 * math.cos(pp) if lateral else 0.0
        p.leg(side, thigh, knee, net, toe=-0.7 * max(net, 0.0) if uu < 0.5 else 0.0, spread=spread)
        swing = arm_sw * math.cos(pp) * fwd_scale
        p.arm(side, swing + (12 if crouch else 0), 10 if run else 8, elbow=elbow, hand=-10 if run else -6)
        p.fingers(side, 35 if run else 16, per=(0.8, 1.0, 1.1, 1.2))
    drop = b.drop(thigh0, k0) if crouch else base
    p.loc = np.array([0, 0, -drop - bob * math.cos(2 * ph)])
    if run:
        p.face(mouthStretch_L=0.15, mouthStretch_R=0.15)


def make_gait(kind, direction="fwd"):
    return lambda b, p, u: gait(b, p, u, kind, direction)


def blend_samplers(sa, sb, w_fn):
    """Pose blend between two samplers with weight w_fn(u) toward sb (starts, stops)."""

    def f(b, p, u):
        pa, pb = Pose(p.hand_axes, p.arm_rest), Pose(p.hand_axes, p.arm_rest)
        sa(b, pa, u)
        sb(b, pb, u)
        w = w_fn(u)
        for k in set(pa.q) | set(pb.q):
            qa = pa.q.get(k, np.array([0, 0, 0, 1.0]))
            qb = pb.q.get(k, np.array([0, 0, 0, 1.0]))
            if np.dot(qa, qb) < 0:
                qb = -qb
            q = qa * (1 - w) + qb * w
            p.q[k] = q / np.linalg.norm(q)
        p.loc = pa.loc * (1 - w) + pb.loc * w
        for k in set(pa.morph) | set(pb.morph):
            p.morph[k] = pa.morph.get(k, 0) * (1 - w) + pb.morph.get(k, 0) * w

    return f


def start(kind):
    g = make_gait(kind)
    return blend_samplers(lambda b, p, u: idle(b, p, 0.0), lambda b, p, u: g(b, p, (u * 1.0) % 1.0),
                          lambda u: smooth(u / 0.7))


def stop(kind):
    g = make_gait(kind)
    return blend_samplers(lambda b, p, u: g(b, p, (0.5 + u * 0.6) % 1.0), lambda b, p, u: idle(b, p, 0.0),
                          lambda u: smooth(u / 0.8))


def turn(direction):
    s = 1.0 if direction == "left" else -1.0

    def f(b, p, u):
        # In-place 90 degree turn in two steps; the yaw accumulates on the Hips (root motion-free
        # clips: gameplay rotates the character, the clip sells the footwork).
        k = smooth(u)
        step1 = bump(u, 0.0, 0.25, 0.3, 0.5)
        step2 = bump(u, 0.45, 0.7, 0.75, 1.0)
        p.rot("Hips", yaw(s * 90 * k))
        p.rot("Spine", yaw(s * 10 * (1 - k) * min(1, u * 4)))
        p.rot("Head", yaw(s * 25 * bump(u, 0, 0.15, 0.5, 0.9)))
        _relaxed(p)
        lead, trail = (1, -1) if s > 0 else (-1, 1)
        p.leg(lead, -4 - 25 * step1, 8 + 45 * step1, 0, turn=s * 30 * (1 - k))
        p.leg(trail, -4 - 25 * step2, 8 + 45 * step2, 0, turn=-s * 20 * (1 - k))
        p.loc = np.array([0, 0, -b.drop(-4, 8) - 0.012 * (step1 + step2)])

    return f


def jump(b, p, u):
    k = ramp(u, 0.1, 0.4)
    crouch = max(0.0, math.sin(min(u / 0.2, 1.0) * math.pi))
    p.rot("Spine", rx(10 * crouch - 6 * k))
    p.rot("Chest", rx(4 * crouch - 4 * k))
    p.rot("Head", rx(-8 * k))
    p.arm(1, 20 * crouch - 30 * k, 8 + 50 * k, elbow=-12 - 30 * k)
    p.arm(-1, 20 * crouch - 30 * k, 8 + 50 * k, elbow=-12 - 30 * k)
    p.fingers(1, 10 + 20 * k)
    p.fingers(-1, 10 + 20 * k)
    p.leg(1, -25 * crouch - 40 * k, 40 * crouch + 70 * k, -10 * k)
    p.leg(-1, -25 * crouch - 20 * k, 40 * crouch + 45 * k, 5 * k)
    p.loc = np.array([0, 0, -b.drop(-25 * crouch, 40 * crouch) + 0.02 * k])
    p.face(eyeWide_L=0.3 * k, eyeWide_R=0.3 * k, mouthStretch_L=0.2 * crouch, mouthStretch_R=0.2 * crouch)


def fall(b, p, u):
    f = math.sin(2 * math.pi * u)
    p.rot("Spine", rx(-4 + 1.5 * f))
    p.rot("Chest", rx(-3))
    p.rot("Head", rx(6 + 2 * f))
    p.arm(1, -18 + 6 * f, 52 + 4 * f, elbow=-25, hand=4)
    p.arm(-1, -18 - 6 * f, 52 - 4 * f, elbow=-25, hand=4)
    p.fingers(1, 8, spread=10)
    p.fingers(-1, 8, spread=10)
    p.leg(1, -28 + 4 * f, 38, -12)
    p.leg(-1, -8 - 4 * f, 22, -6)
    p.face(eyeWide_L=0.6, eyeWide_R=0.6, browInnerUp_L=0.5, browInnerUp_R=0.5, jawOpenStretch=0.1)


def land(b, p, u, hard=False):
    m = 1.6 if hard else 1.0
    imp = smooth(u / 0.22) if u < 0.22 else 1 - 0.85 * smooth((u - 0.22) / 0.78)
    thigh, knee = (-42 * imp - 4) * min(m, 1.35), (80 * imp + 8) * min(m, 1.35)
    p.rot("Spine", rx(16 * imp * m + 1))
    p.rot("Chest", rx(5 * imp * m))
    p.rot("Head", rx(-6 * imp + 2))
    p.arm(1, -14 * imp + 3, 14 + 12 * imp * m, elbow=-12 - 25 * imp)
    p.arm(-1, -14 * imp + 3, 14 + 12 * imp * m, elbow=-12 - 25 * imp)
    p.fingers(1, 20 * imp + 10)
    p.fingers(-1, 20 * imp + 10)
    p.leg(1, thigh, knee, 0)
    p.leg(-1, thigh, knee, 0)
    p.loc = np.array([0, 0, -b.drop(thigh, knee)])
    p.face(eyeSquint_L=0.4 * imp, eyeSquint_R=0.4 * imp, mouthFrown_L=0.3 * imp * (m - 0.6),
           mouthFrown_R=0.3 * imp * (m - 0.6))


def crouch_idle(b, p, u):
    ph = 2 * math.pi * u
    breath = math.sin(2 * ph)
    p.rot("Spine", rx(14))
    p.rot("Chest", rx(10 - 1.0 * breath))
    p.rot("Neck", rx(-10))
    p.rot("Head", rx(-12), yaw(8 * math.sin(ph)))
    p.arm(1, -10, 12, elbow=-45, hand=-10)
    p.arm(-1, -10, 12, elbow=-45, hand=-10)
    p.fingers(1, 25)
    p.fingers(-1, 25)
    p.leg(1, -62, 105, 0)
    p.leg(-1, -50, 95, 0, spread=4)
    p.loc = np.array([0, 0, -b.drop(-56, 100)])
    _blink(p, u, 0.6)


def crouch_transition(enter):
    def f(b, p, u):
        w = smooth(u) if enter else 1 - smooth(u)
        pa, pb = Pose(p.hand_axes, p.arm_rest), Pose(p.hand_axes, p.arm_rest)
        idle(b, pa, 0.0)
        crouch_idle(b, pb, 0.0)
        for k in set(pa.q) | set(pb.q):
            qa = pa.q.get(k, np.array([0, 0, 0, 1.0]))
            qb = pb.q.get(k, np.array([0, 0, 0, 1.0]))
            if np.dot(qa, qb) < 0:
                qb = -qb
            q = qa * (1 - w) + qb * w
            p.q[k] = q / np.linalg.norm(q)
        p.loc = pa.loc * (1 - w) + pb.loc * w

    return f


def interact(b, p, u):
    e = bump(u, 0.05, 0.38, 0.62, 0.98)
    idle(b, p, 0.0)
    p.q.clear()
    p.rot("Chest", rx(4 * e), yaw(-10 * e))
    p.rot("Head", rx(8 * e), yaw(-12 * e))
    p.arm(1, 3, 9, elbow=-12, hand=-5)
    p.fingers(1, 14)
    p.arm(-1, 3 - 72 * e, 9 + 6 * e, elbow=-12 - 20 * e, hand=-5 + 12 * e, twist=-10 * e)
    # Index finger extends to press, the others curl.
    p.fingers(-1, 14 + 45 * e, thumb=20 + 20 * e, per=(0.0 if e > 0.5 else 1.0, 1.0, 1.1, 1.2))
    p.leg(1, -4, 8, 0)
    p.leg(-1, -4, 8, 0)
    p.loc = np.array([0, 0, -b.drop(-4, 8)])
    p.face(browInnerUp_L=0.2 * e, browInnerUp_R=0.2 * e)


def pickup(b, p, u):
    e = bump(u, 0.05, 0.4, 0.6, 0.95)
    p.rot("Spine", rx(25 * e))
    p.rot("Chest", rx(18 * e))
    p.rot("Head", rx(-15 * e))
    p.arm(1, 3, 9, elbow=-12 - 20 * e)
    p.arm(-1, 3 - 40 * e, 6, elbow=-12 - 15 * e, hand=-5)
    p.fingers(1, 14)
    grab = ramp(u, 0.35, 0.45)
    p.fingers(-1, 14 + 60 * grab * (1 - ramp(u, 0.9, 1.0)))
    p.leg(1, -55 * e - 4, 95 * e + 8, 0)
    p.leg(-1, -40 * e - 4, 70 * e + 8, 0)
    p.loc = np.array([0, 0, -b.drop(-50 * e - 4, 85 * e + 8)])


def wave(b, p, u):
    idle(b, p, u * 0.3)
    up = bump(u, 0.0, 0.18, 0.82, 1.0)
    osc = math.sin(2 * math.pi * u * 4) * up
    p.q.pop("UpperArm.R", None)
    p.q.pop("LowerArm.R", None)
    p.q.pop("Hand.R", None)
    for n in [k for k in p.q if k.endswith(".R") and k[:-2] in
              ("Thumb1", "Thumb2", "Thumb3", "Index1", "Index2", "Index3", "Middle1", "Middle2", "Middle3",
               "Ring1", "Ring2", "Ring3", "Pinky1", "Pinky2", "Pinky3")]:
        p.q.pop(n)
    p.arm(-1, -20 * up, 90 * up, elbow=-70 * up - 12 * (1 - up), hand=0, wrist_side=-25 * osc)
    p.fingers(-1, 4)
    p.face(mouthSmile_L=0.6 * up, mouthSmile_R=0.6 * up, cheekSquint_L=0.3 * up, cheekSquint_R=0.3 * up,
           browOuterUp_L=0.2 * up, browOuterUp_R=0.2 * up)


def point(b, p, u):
    idle(b, p, u * 0.2)
    e = bump(u, 0.0, 0.25, 0.75, 1.0)
    for k in [k for k in p.q if k.endswith(".R") and not k.startswith(("UpperLeg", "LowerLeg", "Foot", "Toe", "Shoulder"))]:
        p.q.pop(k)
    p.arm(-1, -80 * e + 3 * (1 - e), 9 + 6 * e, elbow=-10, hand=0)
    p.fingers(-1, 14 + 60 * e, thumb=30 * e + 10, per=(1 - e, 1.0, 1.05, 1.1))
    p.rot("Head", yaw(-10 * e))


def grip(side):
    def f(b, p, u):
        idle(b, p, u)
        t = "L" if side > 0 else "R"
        for k in [k for k in p.q if k.endswith("." + t) and k[:-2] in
                  ("Thumb1", "Thumb2", "Thumb3", "Index1", "Index2", "Index3", "Middle1", "Middle2", "Middle3",
                   "Ring1", "Ring2", "Ring3", "Pinky1", "Pinky2", "Pinky3")]:
            p.q.pop(k)
        p.fingers(side, 72, thumb=55)

    return f


def aim(b, p, u):
    ph = 2 * math.pi * u
    breath = math.sin(2 * ph)
    p.rot("Spine", rx(4), yaw(-15))
    p.rot("Chest", rx(2 - breath), yaw(-10))
    p.rot("Head", yaw(25), rx(4))
    p.arm(-1, -78, 12, elbow=-8, hand=0, twist=-60)
    p.arm(1, -60, -10, elbow=-55, hand=0, twist=40)
    p.fingers(-1, 65, thumb=40, per=(0.2, 1, 1, 1))
    p.fingers(1, 55, thumb=40)
    p.leg(1, -10, 14, 0, spread=4)
    p.leg(-1, 6, 10, 0, spread=4)
    p.loc = np.array([0, 0, -b.drop(-10, 14)])
    p.face(eyeSquint_L=0.25, eyeSquint_R=0.25, browDown_L=0.3, browDown_R=0.3)


# ---- facial -------------------------------------------------------------------------------


def face_blink(b, p, u):
    _blink(p, u, 0.2, 0.25)
    _blink(p, u, 0.7, 0.25)


VISEMES = (  # (jaw degrees, morphs) roughly A, E, O, U, M/B/P, F/V, rest
    (14, {"mouthStretch_L": 0.2, "mouthStretch_R": 0.2}),
    (8, {"mouthStretch_L": 0.5, "mouthStretch_R": 0.5, "mouthSmile_L": 0.2, "mouthSmile_R": 0.2}),
    (11, {"mouthPucker": 0.55}),
    (5, {"mouthPucker": 0.85}),
    (0, {"mouthRollUpper": 0.4, "mouthRollLower": 0.5}),
    (3, {"mouthRollLower": 0.6, "mouthUpperUp": 0.3}),
    (2, {}),
)


def face_talk(b, p, u):
    seq = (0, 4, 1, 2, 6, 3, 5, 0, 1, 4, 2, 6)
    x = u * len(seq)
    i = int(x) % len(seq)
    t = smooth(x - int(x))
    a, c = VISEMES[seq[i]], VISEMES[seq[(i + 1) % len(seq)]]
    jaw = a[0] * (1 - t) + c[0] * t
    p.rot("Jaw", rx(-jaw))
    for k in set(a[1]) | set(c[1]):
        p.face(**{k: a[1].get(k, 0) * (1 - t) + c[1].get(k, 0) * t})
    p.face(browInnerUp_L=0.15 * math.sin(2 * math.pi * u) ** 2, browInnerUp_R=0.15 * math.sin(2 * math.pi * u) ** 2)
    _blink(p, u, 0.55, 0.12)


def face_expression(**w):
    def f(b, p, u):
        k = bump(u, 0.0, 0.25, 0.75, 1.0)
        p.face(**{n: v * k for n, v in w.items()})
        if "jaw" in w:
            p.morph.pop("jaw")
            p.rot("Jaw", rx(-w["jaw"] * k))

    return f


# name, seconds, sampler, loop, root motion speed (m/s along -Y, informational for controllers)
CLIPS = [
    ("Idle", 4.0, idle, True),
    ("Walk", 0.8, make_gait("walk"), True),
    ("Run", 0.55, make_gait("run"), True),
    ("Jump", 0.6, jump, False),
    ("Fall", 0.8, fall, True),
    ("Land", 0.4, land, False),
    ("Interact", 0.9, interact, False),
    # --- extended library (ADR-0179) ---
    ("Idle_Look", 6.0, idle_look, True),
    ("Idle_Shift", 5.0, idle_shift, True),
    ("Walk_Back", 0.85, make_gait("walk", "back"), True),
    ("Walk_Left", 0.85, make_gait("walk", "left"), True),
    ("Walk_Right", 0.85, make_gait("walk", "right"), True),
    ("Run_Back", 0.6, make_gait("run", "back"), True),
    ("Run_Left", 0.6, make_gait("run", "left"), True),
    ("Run_Right", 0.6, make_gait("run", "right"), True),
    ("Sprint", 0.48, make_gait("sprint"), True),
    ("Walk_Start", 0.5, start("walk"), False),
    ("Walk_Stop", 0.6, stop("walk"), False),
    ("Run_Start", 0.45, start("run"), False),
    ("Run_Stop", 0.7, stop("run"), False),
    ("Turn_Left_90", 0.9, turn("left"), False),
    ("Turn_Right_90", 0.9, turn("right"), False),
    ("Land_Hard", 0.8, lambda b, p, u: land(b, p, u, hard=True), False),
    ("Crouch_Idle", 4.0, crouch_idle, True),
    ("Crouch_Walk", 1.1, make_gait("crouch"), True),
    ("Crouch_Walk_Back", 1.1, make_gait("crouch", "back"), True),
    ("Crouch_Walk_Left", 1.1, make_gait("crouch", "left"), True),
    ("Crouch_Walk_Right", 1.1, make_gait("crouch", "right"), True),
    ("Crouch_Enter", 0.35, crouch_transition(True), False),
    ("Crouch_Exit", 0.35, crouch_transition(False), False),
    ("Pickup", 1.4, pickup, False),
    ("Wave", 2.0, wave, False),
    ("Point", 1.6, point, False),
    ("Aim", 3.0, aim, True),
    ("Grip_L", 1.0, grip(1), True),
    ("Grip_R", 1.0, grip(-1), True),
    ("Face_Blink", 3.0, face_blink, True),
    ("Face_Talk", 2.4, face_talk, True),
    ("Face_Smile", 1.5, face_expression(mouthSmile_L=0.9, mouthSmile_R=0.9, cheekSquint_L=0.5, cheekSquint_R=0.5,
                                        eyeSquint_L=0.25, eyeSquint_R=0.25), False),
    ("Face_Frown", 1.5, face_expression(mouthFrown_L=0.8, mouthFrown_R=0.8, browDown_L=0.7, browDown_R=0.7,
                                        noseSneer=0.3), False),
    ("Face_Surprise", 1.5, face_expression(eyeWide_L=0.9, eyeWide_R=0.9, browInnerUp_L=0.8, browInnerUp_R=0.8,
                                           browOuterUp_L=0.8, browOuterUp_R=0.8, jaw=12), False),
]

# Clips whose channels only touch these joint groups play on layers (docs/ANIMATION.md).
FACE_ONLY = {"Face_Blink", "Face_Talk", "Face_Smile", "Face_Frown", "Face_Surprise"}
