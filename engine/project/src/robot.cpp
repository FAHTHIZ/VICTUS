// SPDX-License-Identifier: Apache-2.0
#include "robot.hpp"

#include "victus/animation/animation.hpp"
#include "victus/core/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <vector>

namespace victus::project::robot {

using math::Quat;
using math::Vec3;

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = kPi / 180.0f;

struct Node {
    const char* name;
    int parent;
    Vec3 t;
};

// Node 0 holds the skinned mesh; nodes 1..11 are the joints (skin joint j = node j + 1).
// The robot faces +Z with its feet at y = 0 and is about 1.67 m tall.
constexpr Node kNodes[] = {
    {"Body", -1, {0, 0, 0}},
    {"Hips", -1, {0, 0.78f, 0}},
    {"Spine", 1, {0, 0.08f, 0}},
    {"Head", 2, {0, 0.52f, 0}},
    {"UpperArm.L", 2, {0.33f, 0.44f, 0}},
    {"LowerArm.L", 4, {0, -0.3f, 0}},
    {"UpperArm.R", 2, {-0.33f, 0.44f, 0}},
    {"LowerArm.R", 6, {0, -0.3f, 0}},
    {"UpperLeg.L", 1, {0.13f, -0.06f, 0}},
    {"LowerLeg.L", 8, {0, -0.36f, 0}},
    {"UpperLeg.R", 1, {-0.13f, -0.06f, 0}},
    {"LowerLeg.R", 10, {0, -0.36f, 0}},
};
constexpr int kNodeCount = static_cast<int>(std::size(kNodes));
constexpr int kJointCount = kNodeCount - 1;

int joint(const char* name) {
    for (int i = 1; i < kNodeCount; ++i)
        if (std::strcmp(kNodes[i].name, name) == 0) return i - 1;
    return 0;
}

Vec3 bind_position(int node) {
    Vec3 p{};
    for (int n = node; n >= 0; n = kNodes[n].parent) p = p + kNodes[n].t;
    return p;
}

struct Box {
    Vec3 center, half;
    const char* joint;
    int primitive; // 0 = shell, 1 = accent
};

constexpr Box kBoxes[] = {
    {{0, 0.80f, 0}, {0.2f, 0.08f, 0.13f}, "Hips", 0},
    {{0, 1.10f, 0}, {0.25f, 0.24f, 0.15f}, "Spine", 0},
    {{0, 1.16f, 0.155f}, {0.08f, 0.06f, 0.01f}, "Spine", 1},
    {{0, 1.38f, 0}, {0.06f, 0.04f, 0.06f}, "Head", 1},
    {{0, 1.53f, 0}, {0.16f, 0.14f, 0.15f}, "Head", 0},
    {{0, 1.55f, 0.155f}, {0.12f, 0.04f, 0.01f}, "Head", 1},
    {{0.33f, 1.16f, 0}, {0.065f, 0.15f, 0.065f}, "UpperArm.L", 0},
    {{0.33f, 0.87f, 0}, {0.06f, 0.14f, 0.06f}, "LowerArm.L", 1},
    {{-0.33f, 1.16f, 0}, {0.065f, 0.15f, 0.065f}, "UpperArm.R", 0},
    {{-0.33f, 0.87f, 0}, {0.06f, 0.14f, 0.06f}, "LowerArm.R", 1},
    {{0.13f, 0.55f, 0}, {0.08f, 0.17f, 0.08f}, "UpperLeg.L", 0},
    {{0.13f, 0.21f, 0}, {0.075f, 0.15f, 0.075f}, "LowerLeg.L", 1},
    {{0.13f, 0.03f, 0.04f}, {0.085f, 0.03f, 0.12f}, "LowerLeg.L", 0},
    {{-0.13f, 0.55f, 0}, {0.08f, 0.17f, 0.08f}, "UpperLeg.R", 0},
    {{-0.13f, 0.21f, 0}, {0.075f, 0.15f, 0.075f}, "LowerLeg.R", 1},
    {{-0.13f, 0.03f, 0.04f}, {0.085f, 0.03f, 0.12f}, "LowerLeg.R", 0},
};

struct Primitive {
    std::vector<float> positions, normals, uvs, weights;
    std::vector<std::uint16_t> joints, indices;
    Vec3 min{1e9f, 1e9f, 1e9f}, max{-1e9f, -1e9f, -1e9f};

    void add_box(const Box& b) {
        const int j = joint(b.joint);
        // Six faces: normal axis, then two tangent axes spanning the face.
        const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        for (int a = 0; a < 3; ++a)
            for (const float sign : {1.0f, -1.0f}) {
                const Vec3 n = axes[a] * sign;
                const Vec3 u = axes[(a + 1) % 3] * sign, v = axes[(a + 2) % 3];
                const Vec3 hn{n.x * b.half.x, n.y * b.half.y, n.z * b.half.z};
                const Vec3 hu{u.x * b.half.x, u.y * b.half.y, u.z * b.half.z};
                const Vec3 hv{v.x * b.half.x, v.y * b.half.y, v.z * b.half.z};
                const auto base = static_cast<std::uint16_t>(positions.size() / 3);
                const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
                for (const auto& c : corners) {
                    const Vec3 p = b.center + hn + hu * c[0] + hv * c[1];
                    positions.insert(positions.end(), {p.x, p.y, p.z});
                    normals.insert(normals.end(), {n.x, n.y, n.z});
                    uvs.insert(uvs.end(), {0.5f + 0.5f * c[0], 0.5f - 0.5f * c[1]});
                    joints.insert(joints.end(), {static_cast<std::uint16_t>(j), 0, 0, 0});
                    weights.insert(weights.end(), {1, 0, 0, 0});
                    min = {std::min(min.x, p.x), std::min(min.y, p.y), std::min(min.z, p.z)};
                    max = {std::max(max.x, p.x), std::max(max.y, p.y), std::max(max.z, p.z)};
                }
                // u x v = n, so (0,1,2) is counter-clockwise seen from outside.
                indices.insert(indices.end(), {base, static_cast<std::uint16_t>(base + 1), static_cast<std::uint16_t>(base + 2),
                                               base, static_cast<std::uint16_t>(base + 2), static_cast<std::uint16_t>(base + 3)});
            }
    }
};

class Writer {
public:
    json::Value accessors = json::Value::array(), views = json::Value::array();

    template <class T>
    int add(const std::vector<T>& data, int component, const char* type, int count, int target = 0) {
        while (bytes_.size() % 4) bytes_.push_back(0);
        const std::size_t offset = bytes_.size();
        const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
        bytes_.insert(bytes_.end(), p, p + data.size() * sizeof(T));
        json::Value view = json::Value::object();
        view.set("buffer", 0);
        view.set("byteOffset", static_cast<std::uint64_t>(offset));
        view.set("byteLength", static_cast<std::uint64_t>(data.size() * sizeof(T)));
        if (target) view.set("target", target);
        views.push(std::move(view));
        json::Value acc = json::Value::object();
        acc.set("bufferView", static_cast<int>(views.as_array().size() - 1));
        acc.set("componentType", component);
        acc.set("count", count);
        acc.set("type", type);
        accessors.push(std::move(acc));
        return static_cast<int>(accessors.as_array().size() - 1);
    }
    json::Value& last() { return accessors.as_array().back(); }
    [[nodiscard]] json::Value buffer() const {
        json::Value b = json::Value::object();
        b.set("byteLength", static_cast<std::uint64_t>(bytes_.size()));
        b.set("uri", "data:application/octet-stream;base64," + base64_encode(bytes_));
        return b;
    }

private:
    std::vector<std::uint8_t> bytes_;
};

constexpr int kFloat = 5126, kUShort = 5123;

json::Value vec(std::initializer_list<float> v) {
    json::Value a = json::Value::array();
    for (const float f : v) a.push(f);
    return a;
}

json::Value material(const char* name, Vec3 color, float metallic, float roughness, Vec3 emissive) {
    json::Value pbr = json::Value::object();
    pbr.set("baseColorFactor", vec({color.x, color.y, color.z, 1.0f}));
    pbr.set("metallicFactor", metallic);
    pbr.set("roughnessFactor", roughness);
    json::Value m = json::Value::object();
    m.set("name", name);
    m.set("pbrMetallicRoughness", std::move(pbr));
    m.set("emissiveFactor", vec({emissive.x, emissive.y, emissive.z}));
    return m;
}

// One looping clip: every channel is sampled from a function of the cycle phase (0..2pi).
struct Track {
    const char* node;
    enum { Rotation, Translation } path;
    std::function<Quat(float)> rotation;
    std::function<Vec3(float)> translation;
};

Quat rx(float deg) { return Quat::axis_angle({1, 0, 0}, deg * kDeg); }
Quat ry(float deg) { return Quat::axis_angle({0, 1, 0}, deg * kDeg); }
Quat rz(float deg) { return Quat::axis_angle({0, 0, 1}, deg * kDeg); }

std::vector<Track> idle_tracks() {
    return {
        {"Hips", Track::Translation, {}, [](float p) { return Vec3{0, 0.78f - 0.012f * (0.5f - 0.5f * std::cos(p)), 0}; }},
        {"Spine", Track::Rotation, [](float p) { return rx(2.5f * std::sin(p)); }, {}},
        {"Head", Track::Rotation, [](float p) { return ry(12.0f * std::sin(p)) * rx(-3.0f * std::sin(2 * p)); }, {}},
        {"UpperArm.L", Track::Rotation, [](float p) { return rz(5.0f + 2.0f * std::sin(p)); }, {}},
        {"UpperArm.R", Track::Rotation, [](float p) { return rz(-5.0f - 2.0f * std::sin(p)); }, {}},
        {"LowerArm.L", Track::Rotation, [](float) { return rx(-12.0f); }, {}},
        {"LowerArm.R", Track::Rotation, [](float) { return rx(-12.0f); }, {}},
    };
}

// Positive X rotation swings a hanging limb backwards (-Z); the robot walks towards +Z.
std::vector<Track> walk_tracks() {
    return {
        {"Hips", Track::Translation, {}, [](float p) { return Vec3{0, 0.75f - 0.025f * std::cos(2 * p), 0}; }},
        {"Hips", Track::Rotation, [](float p) { return ry(-6.0f * std::cos(p)); }, {}},
        {"Spine", Track::Rotation, [](float p) { return ry(9.0f * std::cos(p)) * rx(4.0f); }, {}},
        {"UpperLeg.L", Track::Rotation, [](float p) { return rx(-28.0f * std::cos(p)); }, {}},
        {"UpperLeg.R", Track::Rotation, [](float p) { return rx(28.0f * std::cos(p)); }, {}},
        {"LowerLeg.L", Track::Rotation, [](float p) { return rx(6.0f + 40.0f * std::max(0.0f, -std::sin(p))); }, {}},
        {"LowerLeg.R", Track::Rotation, [](float p) { return rx(6.0f + 40.0f * std::max(0.0f, std::sin(p))); }, {}},
        {"UpperArm.L", Track::Rotation, [](float p) { return rx(24.0f * std::cos(p)) * rz(5.0f); }, {}},
        {"UpperArm.R", Track::Rotation, [](float p) { return rx(-24.0f * std::cos(p)) * rz(-5.0f); }, {}},
        {"LowerArm.L", Track::Rotation, [](float p) { return rx(-30.0f - 10.0f * std::cos(p)); }, {}},
        {"LowerArm.R", Track::Rotation, [](float p) { return rx(-30.0f + 10.0f * std::cos(p)); }, {}},
    };
}

// Same footfall phase as Walk (left leg forward at phase 0) so the two blend in sync: longer
// strides, a forward lean, a higher bob and pumping arms.
std::vector<Track> run_tracks() {
    return {
        {"Hips", Track::Translation, {}, [](float p) { return Vec3{0, 0.72f - 0.05f * std::cos(2 * p), 0}; }},
        {"Hips", Track::Rotation, [](float p) { return ry(-9.0f * std::cos(p)); }, {}},
        {"Spine", Track::Rotation, [](float p) { return ry(12.0f * std::cos(p)) * rx(14.0f); }, {}},
        {"Head", Track::Rotation, [](float) { return rx(-10.0f); }, {}},
        {"UpperLeg.L", Track::Rotation, [](float p) { return rx(-10.0f - 45.0f * std::cos(p)); }, {}},
        {"UpperLeg.R", Track::Rotation, [](float p) { return rx(-10.0f + 45.0f * std::cos(p)); }, {}},
        {"LowerLeg.L", Track::Rotation, [](float p) { return rx(15.0f + 75.0f * std::max(0.0f, -std::sin(p))); }, {}},
        {"LowerLeg.R", Track::Rotation, [](float p) { return rx(15.0f + 75.0f * std::max(0.0f, std::sin(p))); }, {}},
        {"UpperArm.L", Track::Rotation, [](float p) { return rx(45.0f * std::cos(p)) * rz(8.0f); }, {}},
        {"UpperArm.R", Track::Rotation, [](float p) { return rx(-45.0f * std::cos(p)) * rz(-8.0f); }, {}},
        {"LowerArm.L", Track::Rotation, [](float) { return rx(-80.0f); }, {}},
        {"LowerArm.R", Track::Rotation, [](float) { return rx(-80.0f); }, {}},
    };
}

// One-shot take-off (played without looping, it holds the last, tucked frame while airborne):
// a quick crouch, then legs tuck and arms rise over the first 40% of the clip.
std::vector<Track> jump_tracks() {
    // Phase 0..2pi maps to clip time 0..1; k eases 0 -> 1 over t in [0.1, 0.4].
    const auto k = [](float p) {
        const float t = std::clamp((p / (2 * kPi) - 0.1f) / 0.3f, 0.0f, 1.0f);
        return t * t * (3 - 2 * t);
    };
    const auto crouch = [](float p) { return std::max(0.0f, std::sin(std::min(p / (2 * kPi) / 0.2f, 1.0f) * kPi)); };
    return {
        {"Hips", Track::Translation, {}, [=](float p) { return Vec3{0, 0.78f - 0.08f * crouch(p) + 0.02f * k(p), 0}; }},
        {"Spine", Track::Rotation, [=](float p) { return rx(10.0f * crouch(p) - 6.0f * k(p)); }, {}},
        {"Head", Track::Rotation, [=](float p) { return rx(-8.0f * k(p)); }, {}},
        {"UpperLeg.L", Track::Rotation, [=](float p) { return rx(-25.0f * crouch(p) - 40.0f * k(p)); }, {}},
        {"UpperLeg.R", Track::Rotation, [=](float p) { return rx(-25.0f * crouch(p) - 20.0f * k(p)); }, {}},
        {"LowerLeg.L", Track::Rotation, [=](float p) { return rx(40.0f * crouch(p) + 70.0f * k(p)); }, {}},
        {"LowerLeg.R", Track::Rotation, [=](float p) { return rx(40.0f * crouch(p) + 45.0f * k(p)); }, {}},
        {"UpperArm.L", Track::Rotation, [=](float p) { return rx(20.0f * crouch(p) - 30.0f * k(p)) * rz(5.0f + 55.0f * k(p)); }, {}},
        {"UpperArm.R", Track::Rotation, [=](float p) { return rx(20.0f * crouch(p) - 30.0f * k(p)) * rz(-5.0f - 55.0f * k(p)); }, {}},
        {"LowerArm.L", Track::Rotation, [=](float p) { return rx(-12.0f - 30.0f * k(p)); }, {}},
        {"LowerArm.R", Track::Rotation, [=](float p) { return rx(-12.0f - 30.0f * k(p)); }, {}},
    };
}

json::Value clip(Writer& w, const char* name, float duration, const std::vector<Track>& tracks) {
    constexpr int kKeys = 25;
    std::vector<float> times;
    for (int k = 0; k < kKeys; ++k) times.push_back(duration * static_cast<float>(k) / static_cast<float>(kKeys - 1));
    const int input = w.add(times, kFloat, "SCALAR", kKeys);
    w.last().set("min", vec({0.0f}));
    w.last().set("max", vec({duration}));
    json::Value samplers = json::Value::array(), channels = json::Value::array();
    for (const Track& t : tracks) {
        std::vector<float> values;
        Quat prev{0, 0, 0, 1};
        for (int k = 0; k < kKeys; ++k) {
            const float phase = 2 * kPi * static_cast<float>(k) / static_cast<float>(kKeys - 1);
            if (t.path == Track::Rotation) {
                Quat q = t.rotation(phase);
                // Keep consecutive keys in the same hemisphere (glTF: v_k != -v_k+1).
                if (q.x * prev.x + q.y * prev.y + q.z * prev.z + q.w * prev.w < 0) q = {-q.x, -q.y, -q.z, -q.w};
                prev = q;
                values.insert(values.end(), {q.x, q.y, q.z, q.w});
            } else {
                const Vec3 v = t.translation(phase);
                values.insert(values.end(), {v.x, v.y, v.z});
            }
        }
        const bool rot = t.path == Track::Rotation;
        const int output = w.add(values, kFloat, rot ? "VEC4" : "VEC3", kKeys);
        json::Value sampler = json::Value::object();
        sampler.set("input", input);
        sampler.set("output", output);
        sampler.set("interpolation", "LINEAR");
        samplers.push(std::move(sampler));
        json::Value target = json::Value::object();
        target.set("node", joint(t.node) + 1);
        target.set("path", rot ? "rotation" : "translation");
        json::Value channel = json::Value::object();
        channel.set("sampler", static_cast<int>(samplers.as_array().size() - 1));
        channel.set("target", std::move(target));
        channels.push(std::move(channel));
    }
    json::Value a = json::Value::object();
    a.set("name", name);
    a.set("samplers", std::move(samplers));
    a.set("channels", std::move(channels));
    return a;
}

} // namespace

std::string gltf() {
    Primitive prims[2];
    for (const Box& b : kBoxes) prims[b.primitive].add_box(b);
    Writer w;
    json::Value primitives = json::Value::array();
    for (int i = 0; i < 2; ++i) {
        Primitive& p = prims[i];
        const int verts = static_cast<int>(p.positions.size() / 3);
        json::Value attrs = json::Value::object();
        attrs.set("POSITION", w.add(p.positions, kFloat, "VEC3", verts, 34962));
        w.last().set("min", vec({p.min.x, p.min.y, p.min.z}));
        w.last().set("max", vec({p.max.x, p.max.y, p.max.z}));
        attrs.set("NORMAL", w.add(p.normals, kFloat, "VEC3", verts, 34962));
        attrs.set("TEXCOORD_0", w.add(p.uvs, kFloat, "VEC2", verts, 34962));
        attrs.set("JOINTS_0", w.add(p.joints, kUShort, "VEC4", verts, 34962));
        attrs.set("WEIGHTS_0", w.add(p.weights, kFloat, "VEC4", verts, 34962));
        json::Value prim = json::Value::object();
        prim.set("attributes", std::move(attrs));
        prim.set("indices", w.add(p.indices, kUShort, "SCALAR", static_cast<int>(p.indices.size()), 34963));
        prim.set("material", i);
        primitives.push(std::move(prim));
    }
    // Inverse bind matrices: the rest pose has no rotation, so each is a translation by -bind.
    std::vector<float> ibm;
    for (int n = 1; n < kNodeCount; ++n) {
        const Vec3 b = bind_position(n);
        ibm.insert(ibm.end(), {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -b.x, -b.y, -b.z, 1});
    }
    const int ibm_accessor = w.add(ibm, kFloat, "MAT4", kJointCount);

    json::Value nodes = json::Value::array(), roots = json::Value::array(), joints = json::Value::array();
    for (int i = 0; i < kNodeCount; ++i) {
        json::Value n = json::Value::object();
        n.set("name", kNodes[i].name);
        if (kNodes[i].t.x != 0 || kNodes[i].t.y != 0 || kNodes[i].t.z != 0)
            n.set("translation", vec({kNodes[i].t.x, kNodes[i].t.y, kNodes[i].t.z}));
        json::Value children = json::Value::array();
        for (int c = 0; c < kNodeCount; ++c)
            if (kNodes[c].parent == i) children.push(c);
        if (!children.as_array().empty()) n.set("children", std::move(children));
        if (i == 0) {
            n.set("mesh", 0);
            n.set("skin", 0);
        }
        if (kNodes[i].parent < 0) roots.push(i);
        if (i > 0) joints.push(i);
        nodes.push(std::move(n));
    }
    json::Value skin = json::Value::object();
    skin.set("name", "Robot");
    skin.set("joints", std::move(joints));
    skin.set("inverseBindMatrices", ibm_accessor);
    skin.set("skeleton", 1);

    json::Value animations = json::Value::array();
    animations.push(clip(w, "Idle", 2.4f, idle_tracks()));
    animations.push(clip(w, "Walk", 0.8f, walk_tracks()));
    animations.push(clip(w, "Run", 0.55f, run_tracks()));
    animations.push(clip(w, "Jump", 0.6f, jump_tracks()));

    json::Value doc = json::Value::object();
    json::Value asset = json::Value::object();
    asset.set("version", "2.0");
    asset.set("generator", "VICTUS template robot");
    doc.set("asset", std::move(asset));
    json::Value scene = json::Value::object();
    scene.set("nodes", std::move(roots));
    json::Value scenes = json::Value::array();
    scenes.push(std::move(scene));
    doc.set("scene", 0);
    doc.set("scenes", std::move(scenes));
    doc.set("nodes", std::move(nodes));
    json::Value mesh = json::Value::object();
    mesh.set("name", "Robot");
    mesh.set("primitives", std::move(primitives));
    json::Value meshes = json::Value::array();
    meshes.push(std::move(mesh));
    doc.set("meshes", std::move(meshes));
    json::Value materials = json::Value::array();
    materials.push(material("Shell", {0.82f, 0.84f, 0.88f}, 0.2f, 0.38f, {0, 0, 0}));
    materials.push(material("Accent", {1.0f, 0.42f, 0.08f}, 0.0f, 0.5f, {0.35f, 0.1f, 0.01f}));
    doc.set("materials", std::move(materials));
    json::Value skins = json::Value::array();
    skins.push(std::move(skin));
    doc.set("skins", std::move(skins));
    doc.set("animations", std::move(animations));
    doc.set("accessors", std::move(w.accessors));
    doc.set("bufferViews", std::move(w.views));
    json::Value buffers = json::Value::array();
    buffers.push(w.buffer());
    doc.set("buffers", std::move(buffers));
    return json::stringify(doc, false) + "\n";
}

std::string controller() {
    // Locomotion blends Idle/Walk/Run by ground speed (m/s); Air plays the one-shot Jump and holds
    // its tucked last frame until the character lands.
    return R"({
  "version": 1,
  "parameters": [
    { "name": "speed", "type": "float" },
    { "name": "grounded", "type": "bool", "default": true },
    { "name": "jump", "type": "trigger" }
  ],
  "default_state": "Locomotion",
  "states": [
    { "name": "Locomotion", "blend1d": { "parameter": "speed", "points": [
        { "clip": "Idle", "at": 0 }, { "clip": "Walk", "at": 2.5 }, { "clip": "Run", "at": 5 } ] } },
    { "name": "Air", "clip": "Jump", "loop": false }
  ],
  "transitions": [
    { "from": "Locomotion", "to": "Air", "duration": 0.08, "conditions": [ { "parameter": "jump" } ] },
    { "from": "Locomotion", "to": "Air", "duration": 0.2, "offset": 0.4,
      "conditions": [ { "parameter": "grounded", "op": "false" } ] },
    { "from": "Air", "to": "Locomotion", "duration": 0.15, "exit_time": 0.3,
      "conditions": [ { "parameter": "grounded", "op": "true" } ] }
  ]
}
)";
}

std::string meta() {
    json::Value m = json::Value::object();
    m.set("victus_meta", 1);
    m.set("uuid", kModelUuid);
    m.set("importer", "gltf");
    return json::stringify(m, true) + "\n";
}

scene::Entity instantiate(scene::Scene& s, std::string_view name, scene::Entity parent) {
    Uuid model;
    (void)Uuid::parse(kModelUuid, model);
    const scene::Entity root = s.create(name, parent);
    std::vector<scene::Entity> created(kNodeCount);
    for (int i = 0; i < kNodeCount; ++i) {
        const scene::Entity p = kNodes[i].parent < 0 ? root : created[static_cast<std::size_t>(kNodes[i].parent)];
        created[static_cast<std::size_t>(i)] = s.create(kNodes[i].name, p);
        s.world().get<scene::Transform>(created[static_cast<std::size_t>(i)])->position = kNodes[i].t;
    }
    // The body node has two primitives: one child part per primitive, as instantiate_model does.
    for (std::uint32_t mi = 0; mi < 2; ++mi) {
        const scene::Entity part = s.create(std::format("{}[{}]", kNodes[0].name, mi), created[0]);
        auto& mr = s.world().add<scene::MeshRendererComponent>(part);
        mr.mesh_asset = model;
        mr.mesh_index = mi;
        s.world().add<animation::SkinnedMeshComponent>(part, s.uuid(root), 0u);
    }
    auto& a = s.world().add<animation::AnimatorComponent>(root);
    a.model = model;
    a.clip = "Idle";
    a.controller = kControllerAsset;
    return root;
}

} // namespace victus::project::robot
