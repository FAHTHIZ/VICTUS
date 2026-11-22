// SPDX-License-Identifier: Apache-2.0
#include "victus/assets/reference_rigs.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace victus::assets {

using math::Mat4;
using math::Quat;
using math::Vec3;

namespace {

constexpr float kPi = 3.14159265358979f;

struct Builder {
    CookedModel m;
    std::map<std::string, std::uint32_t, std::less<>> node_of;
    std::map<std::string, Vec3, std::less<>> pos; // model-space rest positions

    Builder() {
        m.materials.push_back({});
        m.materials.back().name = "Rig";
        m.materials.back().metallic = 0.0f;
        m.materials.back().roughness = 0.6f;
        add("Rig", "", {0, 0, 0}, false);
        m.skins.push_back({});
        m.skins.back().name = "Skin";
    }
    // Adds a node at model-space position `p` (identity rest rotations: bind = translation).
    std::uint32_t add(const std::string& name, std::string_view parent, Vec3 p, bool joint = true) {
        ModelNode n;
        n.name = name;
        const auto it = node_of.find(parent);
        n.parent = it == node_of.end() ? -1 : static_cast<std::int32_t>(it->second);
        n.translation = it == node_of.end() ? p : p - pos[std::string(parent)];
        m.nodes.push_back(n);
        const auto idx = static_cast<std::uint32_t>(m.nodes.size() - 1);
        node_of[name] = idx;
        pos[name] = p;
        if (joint) {
            m.skins[0].joints.push_back(idx);
            m.skins[0].inverse_bind.push_back(Mat4::translation(-p));
        }
        return idx;
    }
    std::uint32_t joint(std::string_view name) const {
        const std::uint32_t node = node_of.find(name)->second;
        const auto& j = m.skins[0].joints;
        return static_cast<std::uint32_t>(std::find(j.begin(), j.end(), node) - j.begin());
    }
};

// Closed tube through `pts` (radius `radii`), skinned to `bones[i]` on segment i with a smooth
// blend into the neighbouring bone over `blend` of each segment either side of a joint.
void add_tube(render::MeshData& d, const std::vector<Vec3>& pts, const std::vector<float>& radii,
              const std::vector<std::uint32_t>& bones, std::uint32_t rings_per_segment = 6, std::uint32_t sides = 20,
              float blend = 0.3f) {
    const std::size_t segs = pts.size() - 1;
    const auto first = static_cast<std::uint32_t>(d.vertices.size());
    // Parallel-transport frame along the polyline.
    Vec3 dir = math::normalize(pts[1] - pts[0]);
    Vec3 nrm = std::abs(dir.y) < 0.9f ? math::normalize(math::cross(dir, Vec3{0, 1, 0})) : math::normalize(math::cross(dir, Vec3{1, 0, 0}));
    const std::uint32_t rings = static_cast<std::uint32_t>(segs) * rings_per_segment + 1;
    for (std::uint32_t r = 0; r < rings; ++r) {
        const float u = static_cast<float>(r) / static_cast<float>(rings_per_segment);
        const std::size_t k = std::min<std::size_t>(static_cast<std::size_t>(u), segs - 1);
        const float f = u - static_cast<float>(k);
        const Vec3 c = pts[k] + (pts[k + 1] - pts[k]) * f;
        const float rad = radii[k] + (radii[k + 1] - radii[k]) * f;
        const Vec3 nd = math::normalize(pts[k + 1] - pts[k]);
        // Rotate the frame from the previous direction to this one (minimal twist).
        const Vec3 axis = math::cross(dir, nd);
        const float s = math::length(axis);
        if (s > 1e-6f) nrm = Quat::axis_angle(axis / s, std::atan2(s, math::dot(dir, nd))).rotate(nrm);
        dir = nd;
        const Vec3 bin = math::cross(dir, nrm);
        // Weights: the segment's bone, blended into the previous/next one near the joints.
        std::uint32_t js[2] = {bones[k], bones[k]};
        float ws[2] = {1.0f, 0.0f};
        if (f < blend && k > 0) {
            const float t = 0.5f + 0.5f * f / blend;
            js[1] = bones[k - 1];
            ws[0] = t;
            ws[1] = 1.0f - t;
        } else if (f > 1.0f - blend && k + 1 < segs) {
            const float t = 0.5f + 0.5f * (1.0f - f) / blend;
            js[1] = bones[k + 1];
            ws[0] = t;
            ws[1] = 1.0f - t;
        }
        for (std::uint32_t sdx = 0; sdx < sides; ++sdx) {
            const float a = 2.0f * kPi * static_cast<float>(sdx) / static_cast<float>(sides);
            const Vec3 n = nrm * std::cos(a) + bin * std::sin(a);
            render::Vertex3D v;
            v.position = c + n * rad;
            v.normal = n;
            v.uv = {static_cast<float>(sdx) / static_cast<float>(sides), u / static_cast<float>(segs)};
            d.vertices.push_back(v);
            d.skin.push_back(render::make_skin_vertex(js, ws, ws[1] > 0 ? 2 : 1));
        }
    }
    for (std::uint32_t r = 0; r + 1 < rings; ++r)
        for (std::uint32_t sdx = 0; sdx < sides; ++sdx) {
            const std::uint32_t a = first + r * sides + sdx, b = first + r * sides + (sdx + 1) % sides;
            const std::uint32_t c = a + sides, e = b + sides;
            d.indices.insert(d.indices.end(), {a, c, b, b, c, e});
        }
    // Caps (fans to a centre vertex), rigid on the end bones.
    for (const bool start : {true, false}) {
        const std::uint32_t ring = start ? 0 : rings - 1;
        render::Vertex3D v;
        v.position = start ? pts.front() : pts.back();
        v.normal = start ? -math::normalize(pts[1] - pts[0]) : math::normalize(pts[segs] - pts[segs - 1]);
        const auto centre = static_cast<std::uint32_t>(d.vertices.size());
        d.vertices.push_back(v);
        const std::uint32_t j = start ? bones.front() : bones.back();
        const float w = 1.0f;
        d.skin.push_back(render::make_skin_vertex(&j, &w, 1));
        for (std::uint32_t sdx = 0; sdx < sides; ++sdx) {
            const std::uint32_t a = first + ring * sides + sdx, b = first + ring * sides + (sdx + 1) % sides;
            if (start) d.indices.insert(d.indices.end(), {centre, a, b});
            else d.indices.insert(d.indices.end(), {centre, b, a});
        }
    }
}

// Axis-aligned box (model space), rigid on `joint`.
void add_box(render::MeshData& d, Vec3 lo, Vec3 hi, std::uint32_t joint) {
    const Vec3 n[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const float w = 1.0f;
    for (const Vec3& nn : n) {
        const Vec3 u = std::abs(nn.y) > 0.5f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        const Vec3 v = math::cross(nn, u);
        const Vec3 c = (lo + hi) * 0.5f, h = (hi - lo) * 0.5f;
        const auto base = static_cast<std::uint32_t>(d.vertices.size());
        for (const auto& [a, b] : {std::pair{-1.0f, -1.0f}, std::pair{1.0f, -1.0f}, std::pair{1.0f, 1.0f}, std::pair{-1.0f, 1.0f}}) {
            render::Vertex3D vx;
            const Vec3 p = nn + u * a + v * b;
            vx.position = c + Vec3{p.x * h.x, p.y * h.y, p.z * h.z};
            vx.normal = nn;
            vx.uv = {a * 0.5f + 0.5f, b * 0.5f + 0.5f};
            d.vertices.push_back(vx);
            d.skin.push_back(render::make_skin_vertex(&joint, &w, 1));
        }
        d.indices.insert(d.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
}

// Sampled rotation channel: q(t) for t in [0, duration] at `rate` keys per second.
template <typename Fn>
animation::Channel rot_channel(std::uint32_t node, float duration, Fn&& fn, float rate = 30.0f) {
    animation::Channel ch;
    ch.node = node;
    ch.path = animation::Path::Rotation;
    const int keys = std::max(2, static_cast<int>(std::ceil(duration * rate)) + 1);
    for (int i = 0; i < keys; ++i) {
        const float t = duration * static_cast<float>(i) / static_cast<float>(keys - 1);
        const Quat q = math::normalize(fn(t));
        ch.times.push_back(t);
        ch.values.insert(ch.values.end(), {q.x, q.y, q.z, q.w});
    }
    return ch;
}

Quat rx(float a) { return Quat::axis_angle({1, 0, 0}, a); }
Quat ry(float a) { return Quat::axis_angle({0, 1, 0}, a); }
Quat rz(float a) { return Quat::axis_angle({0, 0, 1}, a); }

CookedModel make_quadruped() {
    Builder b;
    // Spine from the hips (back) to the chest (front), facing +Z.
    b.add("Hips", "Rig", {0, 0.62f, -0.32f});
    b.add("Spine1", "Hips", {0, 0.64f, -0.12f});
    b.add("Spine2", "Spine1", {0, 0.65f, 0.08f});
    b.add("Chest", "Spine2", {0, 0.64f, 0.28f});
    b.add("Neck1", "Chest", {0, 0.72f, 0.42f});
    b.add("Neck2", "Neck1", {0, 0.82f, 0.50f});
    b.add("Head", "Neck2", {0, 0.88f, 0.58f});
    b.add("Jaw", "Head", {0, 0.84f, 0.64f});
    b.add("Ear.L", "Head", {0.07f, 0.97f, 0.58f});
    b.add("Ear.R", "Head", {-0.07f, 0.97f, 0.58f});
    b.add("Socket.Head", "Head", {0, 0.98f, 0.62f}, false);
    b.add("Socket.Back", "Spine2", {0, 0.78f, 0.08f}, false);
    b.add("Dyn.Belly", "Spine2", {0, 0.52f, 0.0f});
    std::string parent = "Hips";
    for (int i = 1; i <= 7; ++i) {
        const std::string n = "Tail" + std::to_string(i);
        b.add(n, parent, {0, 0.64f - 0.01f * static_cast<float>(i), -0.38f - 0.08f * static_cast<float>(i)});
        parent = n;
    }
    for (const char* side : {"L", "R"}) {
        const float x = side[0] == 'L' ? 0.13f : -0.13f;
        const std::string s = std::string(".") + side;
        b.add("FrontShoulder" + s, "Chest", {x, 0.58f, 0.30f});
        b.add("FrontUpperLeg" + s, "FrontShoulder" + s, {x, 0.44f, 0.32f});
        b.add("FrontLowerLeg" + s, "FrontUpperLeg" + s, {x, 0.24f, 0.30f});
        b.add("FrontFoot" + s, "FrontLowerLeg" + s, {x, 0.05f, 0.31f});
        b.add("FrontToe" + s, "FrontFoot" + s, {x, 0.02f, 0.38f});
        b.add("HindThigh" + s, "Hips", {x, 0.58f, -0.34f});
        b.add("HindLowerLeg" + s, "HindThigh" + s, {x, 0.36f, -0.26f});
        b.add("HindHock" + s, "HindLowerLeg" + s, {x, 0.18f, -0.38f});
        b.add("HindFoot" + s, "HindHock" + s, {x, 0.05f, -0.35f});
        b.add("HindToe" + s, "HindFoot" + s, {x, 0.02f, -0.28f});
    }
    const auto P = [&](const char* n) { return b.pos[n]; };
    const auto J = [&](std::string_view n) { return b.joint(n); };
    ModelMesh body;
    body.name = "Body";
    // Body: tail tip -> tail -> hips -> spine -> chest -> neck -> head -> muzzle.
    {
        // Points: tail tip, Tail7..Tail1, Hips, spine, neck, head, muzzle. The segment between
        // Tail(i+1) and Tail(i) belongs to Tail(i) (the bone that spans it); Hips owns both the
        // rump and the first back segment.
        std::vector<Vec3> pts{b.pos["Tail7"] + (b.pos["Tail7"] - b.pos["Tail6"])};
        std::vector<float> radii{0.012f};
        std::vector<std::uint32_t> bones{J("Tail7")};
        for (int i = 7; i >= 1; --i) {
            pts.push_back(b.pos["Tail" + std::to_string(i)]);
            radii.push_back(0.016f + 0.007f * static_cast<float>(7 - i));
            if (i > 1) bones.push_back(J("Tail" + std::to_string(i - 1)));
        }
        bones.push_back(J("Hips"));
        for (const char* n : {"Hips", "Spine1", "Spine2", "Chest", "Neck1", "Neck2", "Head"}) {
            pts.push_back(P(n));
            bones.push_back(J(n));
        }
        radii.insert(radii.end(), {0.15f, 0.16f, 0.17f, 0.16f, 0.09f, 0.075f, 0.07f});
        pts.push_back(P("Head") + Vec3{0, -0.02f, 0.14f}); // muzzle tip
        radii.push_back(0.035f);
        add_tube(body.data, pts, radii, bones);
    }
    // Jaw and ears: small rigid tubes.
    add_tube(body.data, {P("Jaw"), P("Jaw") + Vec3{0, -0.02f, 0.10f}}, {0.03f, 0.018f}, {J("Jaw")}, 3, 12);
    add_tube(body.data, {P("Ear.L"), P("Ear.L") + Vec3{0.02f, 0.07f, -0.02f}}, {0.02f, 0.006f}, {J("Ear.L")}, 3, 10);
    add_tube(body.data, {P("Ear.R"), P("Ear.R") + Vec3{-0.02f, 0.07f, -0.02f}}, {0.02f, 0.006f}, {J("Ear.R")}, 3, 10);
    // Belly: a soft bulge on the jiggle bone.
    add_tube(body.data, {P("Dyn.Belly") + Vec3{0, 0, -0.12f}, P("Dyn.Belly") + Vec3{0, 0, 0.12f}}, {0.07f, 0.07f}, {J("Dyn.Belly")}, 4, 14);
    for (const char* side : {"L", "R"}) {
        const std::string s = std::string(".") + side;
        std::vector<Vec3> fp, hp;
        std::vector<std::uint32_t> fb, hb;
        for (const std::string n : {"FrontShoulder", "FrontUpperLeg", "FrontLowerLeg", "FrontFoot", "FrontToe"}) {
            fp.push_back(b.pos[n + s]);
            fb.push_back(J(n + s));
        }
        fp.push_back(b.pos["FrontToe" + s] + Vec3{0, 0, 0.04f});
        add_tube(body.data, fp, {0.065f, 0.05f, 0.04f, 0.035f, 0.03f, 0.02f}, fb);
        for (const std::string n : {"HindThigh", "HindLowerLeg", "HindHock", "HindFoot", "HindToe"}) {
            hp.push_back(b.pos[n + s]);
            hb.push_back(J(n + s));
        }
        hp.push_back(b.pos["HindToe" + s] + Vec3{0, 0, 0.04f});
        add_tube(body.data, hp, {0.08f, 0.055f, 0.04f, 0.035f, 0.03f, 0.02f}, hb);
    }
    b.m.meshes.push_back(std::move(body));
    b.m.nodes[0].meshes.push_back(0);
    b.m.nodes[0].skin = 0;

    // ---- clips ----
    const auto N = [&](std::string_view n) { return b.node_of.find(n)->second; };
    {
        animation::Clip walk;
        walk.name = "Walk";
        walk.duration = 1.0f;
        const float w = 2.0f * kPi;
        // Diagonal gait: front left with hind right.
        for (const char* side : {"L", "R"}) {
            const std::string s = std::string(".") + side;
            const float ph = side[0] == 'L' ? 0.0f : kPi;
            walk.channels.push_back(rot_channel(N("FrontUpperLeg" + s), 1.0f, [&](float t) { return rx(0.45f * std::sin(w * t + ph)); }));
            walk.channels.push_back(rot_channel(N("FrontLowerLeg" + s), 1.0f, [&](float t) { return rx(-0.5f * std::max(0.0f, std::sin(w * t + ph + 1.2f))); }));
            walk.channels.push_back(rot_channel(N("HindThigh" + s), 1.0f, [&](float t) { return rx(0.4f * std::sin(w * t + ph + kPi)); }));
            walk.channels.push_back(rot_channel(N("HindHock" + s), 1.0f, [&](float t) { return rx(0.6f * std::max(0.0f, std::sin(w * t + ph + kPi + 1.0f))); }));
        }
        walk.channels.push_back(rot_channel(N("Spine1"), 1.0f, [&](float t) { return ry(0.06f * std::sin(w * t)); }));
        walk.channels.push_back(rot_channel(N("Chest"), 1.0f, [&](float t) { return ry(-0.06f * std::sin(w * t)); }));
        walk.channels.push_back(rot_channel(N("Neck1"), 1.0f, [&](float t) { return rx(0.08f * std::sin(2 * w * t)); }));
        walk.channels.push_back(rot_channel(N("Jaw"), 1.0f, [&](float t) { return rx(0.25f * (0.5f + 0.5f * std::sin(w * t))); }));
        for (int i = 1; i <= 7; ++i)
            walk.channels.push_back(rot_channel(N("Tail" + std::to_string(i)), 1.0f, [&, i](float t) {
                return ry(0.18f * std::sin(w * t - 0.5f * static_cast<float>(i))) * rx(0.05f);
            }));
        b.m.animations.push_back(std::move(walk));
    }
    {
        animation::Clip sit;
        sit.name = "Sit";
        sit.duration = 1.0f;
        const auto ease = [](float t) { return t * t * (3 - 2 * t); };
        for (const char* side : {"L", "R"}) {
            const std::string s = std::string(".") + side;
            sit.channels.push_back(rot_channel(N("HindThigh" + s), 1.0f, [&](float t) { return rx(-1.3f * ease(t)); }));
            sit.channels.push_back(rot_channel(N("HindLowerLeg" + s), 1.0f, [&](float t) { return rx(2.2f * ease(t)); }));
            sit.channels.push_back(rot_channel(N("HindHock" + s), 1.0f, [&](float t) { return rx(-1.6f * ease(t)); }));
        }
        sit.channels.push_back(rot_channel(N("Hips"), 1.0f, [&](float t) { return rx(-0.6f * ease(t)); }));
        sit.channels.push_back(rot_channel(N("Chest"), 1.0f, [&](float t) { return rx(0.3f * ease(t)); }));
        for (int i = 1; i <= 7; ++i)
            sit.channels.push_back(rot_channel(N("Tail" + std::to_string(i)), 1.0f, [&](float t) { return ry(0.35f * ease(t)) * rx(0.15f * ease(t)); }));
        b.m.animations.push_back(std::move(sit));
    }
    {
        animation::Clip shake;
        shake.name = "Shake";
        shake.duration = 0.8f;
        for (const char* n : {"Spine1", "Spine2", "Chest", "Neck1", "Neck2", "Head"})
            shake.channels.push_back(rot_channel(N(n), 0.8f, [&](float t) { return rz(0.35f * std::sin(6 * kPi * t) * (1 - t / 0.8f)); }));
        shake.channels.push_back(rot_channel(N("Dyn.Belly"), 0.8f, [&](float t) { return rz(-0.4f * std::sin(6 * kPi * t - 0.6f)); }));
        for (const char* n : {"Ear.L", "Ear.R"})
            shake.channels.push_back(rot_channel(N(n), 0.8f, [&](float t) { return rx(0.6f * std::sin(6 * kPi * t - 0.9f)); }));
        b.m.animations.push_back(std::move(shake));
    }
    return std::move(b.m);
}

CookedModel make_mech() {
    Builder b;
    b.add("Base", "Rig", {0, 0.15f, 0});
    b.add("Mech.Turret", "Base", {0, 0.35f, 0});
    b.add("Mech.Boom", "Mech.Turret", {0, 0.55f, 0.05f});
    b.add("Mech.Stick", "Mech.Boom", {0, 1.05f, 0.55f});
    b.add("Mech.Wrist", "Mech.Stick", {0, 0.75f, 1.05f});
    b.add("Mech.ClawHinge.L", "Mech.Wrist", {0.05f, 0.70f, 1.15f});
    b.add("Mech.ClawHinge.R", "Mech.Wrist", {-0.05f, 0.70f, 1.15f});
    b.add("Mech.Piston.Cylinder", "Mech.Turret", {0, 0.45f, 0.18f});
    b.add("Mech.Piston.Rod", "Mech.Boom", {0, 0.85f, 0.35f});
    b.add("Wheel.FL", "Base", {0.3f, 0.12f, 0.3f});
    b.add("Wheel.FR", "Base", {-0.3f, 0.12f, 0.3f});
    b.add("Wheel.BL", "Base", {0.3f, 0.12f, -0.3f});
    b.add("Wheel.BR", "Base", {-0.3f, 0.12f, -0.3f});
    b.add("Socket.Tool", "Mech.Wrist", {0, 0.68f, 1.2f}, false);
    const auto J = [&](std::string_view n) { return b.joint(n); };
    ModelMesh hull;
    hull.name = "Hull";
    auto& d = hull.data;
    add_box(d, {-0.32f, 0.08f, -0.4f}, {0.32f, 0.28f, 0.4f}, J("Base"));
    add_box(d, {-0.2f, 0.28f, -0.2f}, {0.2f, 0.5f, 0.2f}, J("Mech.Turret"));
    // Boom: from the turret hinge towards the stick hinge (a box stretched along the diagonal is
    // approximated by two boxes).
    add_box(d, {-0.06f, 0.55f, 0.0f}, {0.06f, 0.82f, 0.28f}, J("Mech.Boom"));
    add_box(d, {-0.06f, 0.80f, 0.26f}, {0.06f, 1.08f, 0.58f}, J("Mech.Boom"));
    add_box(d, {-0.05f, 0.76f, 0.55f}, {0.05f, 1.08f, 1.06f}, J("Mech.Stick"));
    add_box(d, {-0.07f, 0.70f, 1.04f}, {0.07f, 0.80f, 1.14f}, J("Mech.Wrist"));
    add_box(d, {0.03f, 0.64f, 1.14f}, {0.07f, 0.74f, 1.3f}, J("Mech.ClawHinge.L"));
    add_box(d, {-0.07f, 0.64f, 1.14f}, {-0.03f, 0.74f, 1.3f}, J("Mech.ClawHinge.R"));
    add_box(d, {-0.03f, 0.43f, 0.15f}, {0.03f, 0.6f, 0.24f}, J("Mech.Piston.Cylinder"));
    add_box(d, {-0.015f, 0.6f, 0.2f}, {0.015f, 0.86f, 0.36f}, J("Mech.Piston.Rod"));
    for (const char* w : {"Wheel.FL", "Wheel.FR", "Wheel.BL", "Wheel.BR"}) {
        const Vec3 p = b.pos[w];
        add_box(d, p - Vec3{0.05f, 0.12f, 0.12f}, p + Vec3{0.05f, 0.12f, 0.12f}, J(w));
    }
    b.m.meshes.push_back(std::move(hull));
    b.m.nodes[0].meshes.push_back(0);
    b.m.nodes[0].skin = 0;
    const auto N = [&](std::string_view n) { return b.node_of.find(n)->second; };
    {
        animation::Clip work;
        work.name = "Work";
        work.duration = 2.0f;
        const float w = kPi; // one cycle per 2 s
        work.channels.push_back(rot_channel(N("Mech.Turret"), 2.0f, [&](float t) { return ry(0.9f * std::sin(w * t)); }));
        work.channels.push_back(rot_channel(N("Mech.Boom"), 2.0f, [&](float t) { return rx(-0.5f + 0.4f * std::sin(w * t + 0.7f)); }));
        work.channels.push_back(rot_channel(N("Mech.Stick"), 2.0f, [&](float t) { return rx(0.8f * std::sin(w * t + 1.4f)); }));
        work.channels.push_back(rot_channel(N("Mech.Wrist"), 2.0f, [&](float t) { return rz(1.5f * std::sin(w * t)); }));
        work.channels.push_back(rot_channel(N("Mech.ClawHinge.L"), 2.0f, [&](float t) { return ry(0.4f * (0.5f + 0.5f * std::sin(2 * w * t))); }));
        work.channels.push_back(rot_channel(N("Mech.ClawHinge.R"), 2.0f, [&](float t) { return ry(-0.4f * (0.5f + 0.5f * std::sin(2 * w * t))); }));
        for (const char* wh : {"Wheel.FL", "Wheel.FR", "Wheel.BL", "Wheel.BR"})
            work.channels.push_back(rot_channel(N(wh), 2.0f, [&](float t) { return rx(4.0f * w * t); }));
        b.m.animations.push_back(std::move(work));
    }
    {
        animation::Clip spin;
        spin.name = "Spin";
        spin.duration = 1.0f;
        spin.channels.push_back(rot_channel(N("Mech.Wrist"), 1.0f, [&](float t) { return rz(2.0f * kPi * t); }, 60.0f));
        b.m.animations.push_back(std::move(spin));
    }
    return std::move(b.m);
}

} // namespace

std::vector<BoneEdit> complex_humanoid_extension() {
    std::vector<BoneEdit> e;
    const auto add = [&](std::string bone, std::string parent, Vec3 t, bool deform = true) {
        BoneEdit b;
        b.op = BoneEdit::Op::Add;
        b.bone = std::move(bone);
        b.parent = std::move(parent);
        b.translation = t;
        b.deform = deform;
        e.push_back(b);
        return &e.back();
    };
    // Facial bones on the head and jaw.
    for (const char* side : {"L", "R"}) {
        const float x = side[0] == 'L' ? 0.03f : -0.03f;
        add(std::string("Brow.") + side, "Head", {x, 0.09f, 0.09f});
        add(std::string("Lid.Upper.") + side, "Head", {x, 0.065f, 0.085f});
        add(std::string("Lip.Corner.") + side, "Jaw", {x * 0.8f, -0.02f, 0.08f});
        // Toes: a second toe segment per foot.
        add(std::string("Toe2.") + side, std::string("Toe.") + side, {0, 0.03f, 0});
        // IK targets and poles: helpers that do not deform.
        add(std::string("IK.Hand.") + side, "Hips", {x * 15.0f, 0.0f, 0.2f}, false);
        add(std::string("Pole.Elbow.") + side, "Hips", {x * 15.0f, 0.2f, -0.4f}, false);
        // A forearm roll joint taking half of the forearm's weights toward the wrist.
        BoneEdit* roll = add(std::string("ForeArmRoll.") + side, std::string("LowerArm.") + side, {0, 0.12f, 0});
        roll->weights_from = std::string("LowerArm.") + side;
        roll->weight_share = 0.5f;
        roll->weight_start = 0.2f;
        roll->weight_end = 1.0f;
    }
    add("Lip.Upper", "Head", {0, -0.01f, 0.095f});
    add("Tongue1", "Jaw", {0, 0.0f, 0.04f});
    // Secondary motion and an equipment bone.
    add("Dyn.Antenna1", "Head", {0.05f, 0.12f, -0.02f});
    add("Dyn.Antenna2", "Dyn.Antenna1", {0, 0.06f, 0});
    add("Socket.Shoulder.R", "Shoulder.R", {0, 0.05f, 0}, false);
    return e;
}

CookedModel make_reference_rig(ReferenceRig rig) { return rig == ReferenceRig::Mech ? make_mech() : make_quadruped(); }

bool parse_reference_rig(std::string_view name, ReferenceRig& out) noexcept {
    if (name == "quadruped" || name == "hound" || name == "creature") {
        out = ReferenceRig::Quadruped;
        return true;
    }
    if (name == "mech" || name == "loader") {
        out = ReferenceRig::Mech;
        return true;
    }
    return false;
}

} // namespace victus::assets
