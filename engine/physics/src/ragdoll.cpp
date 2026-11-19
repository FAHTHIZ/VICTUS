// SPDX-License-Identifier: Apache-2.0
// Ragdoll authoring helpers (ADR-0091): bone geometry shared by the Jolt backend and the editor
// wireframes, and the humanoid auto-setup. The simulation lives in jolt_world.cpp.
#include "internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <numbers>
#include <string>
#include <string_view>

namespace victus::physics {

namespace detail {

void ragdoll_bone_axis(const Pose& self, const Pose* child, const Pose* parent, const RagdollBoneComponent& bone,
                       math::Vec3& dir, float& length) {
    const float radius = std::max(0.005f, bone.radius);
    length = 2.0f * radius;
    dir = {};
    if (child) {
        dir = child->position - self.position;
        length = math::length(dir);
    } else if (parent) {
        dir = self.position - parent->position;
    }
    if (math::length(dir) < 1e-5f) dir = self.rotation.rotate({0, 1, 0});
    dir = math::normalize(dir);
    if (bone.length > 0) length = bone.length;
    length = std::max(length, 0.01f);
}

namespace {

Entity first_child_bone(const scene::Scene& scene, Entity e, int depth = 0) {
    if (depth > 128) return {};
    for (const Entity c : scene.children(e)) {
        if (scene.world().get<RagdollBoneComponent>(c)) return c;
        if (const Entity d = first_child_bone(scene, c, depth + 1); !d.is_null()) return d;
    }
    return {};
}

Entity parent_bone(const scene::Scene& scene, Entity e) {
    for (Entity p = scene.parent(e); !p.is_null(); p = scene.parent(p))
        if (scene.world().get<RagdollBoneComponent>(p)) return p;
    return {};
}

void circle(std::vector<DebugLine>& out, math::Vec3 c, math::Vec3 u, math::Vec3 v, float r, math::Color color) {
    constexpr int seg = 16;
    math::Vec3 prev = c + u * r;
    for (int i = 1; i <= seg; ++i) {
        const float a = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / seg;
        const math::Vec3 p = c + u * (r * std::cos(a)) + v * (r * std::sin(a));
        out.push_back({prev, p, color});
        prev = p;
    }
}

} // namespace

void ragdoll_lines(scene::Scene& scene, std::vector<DebugLine>& out, Entity only) {
    constexpr math::Color kBone{1.0f, 0.55f, 0.35f, 1.0f};
    scene.world().each<RagdollBoneComponent>([&](Entity e, RagdollBoneComponent& bone) {
        if (!only.is_null() && e != only) return;
        const Pose self = world_pose(scene, e);
        const Entity c = first_child_bone(scene, e), p = parent_bone(scene, e);
        Pose child_pose, parent_pose;
        if (!c.is_null()) child_pose = world_pose(scene, c);
        if (!p.is_null()) parent_pose = world_pose(scene, p);
        math::Vec3 dir;
        float len = 0;
        ragdoll_bone_axis(self, c.is_null() ? nullptr : &child_pose, p.is_null() ? nullptr : &parent_pose, bone, dir, len);
        const float r = std::max(0.005f, bone.radius);
        math::Vec3 u = math::cross(dir, std::abs(dir.y) < 0.9f ? math::Vec3{0, 1, 0} : math::Vec3{1, 0, 0});
        u = math::normalize(u);
        const math::Vec3 v = math::cross(dir, u);
        const math::Vec3 mid = self.position + dir * (len * 0.5f);
        if (bone.shape == RagdollBoneComponent::Shape::Sphere) {
            circle(out, mid, u, v, r, kBone);
            circle(out, mid, u, dir, r, kBone);
            circle(out, mid, v, dir, r, kBone);
            return;
        }
        const float half = bone.shape == RagdollBoneComponent::Shape::Capsule ? std::max(len * 0.5f - r, 0.005f) : len * 0.5f;
        const math::Vec3 a = mid - dir * half, b = mid + dir * half;
        circle(out, a, u, v, r, kBone);
        circle(out, b, u, v, r, kBone);
        for (const math::Vec3 d : {u, u * -1.0f, v, v * -1.0f}) out.push_back({a + d * r, b + d * r, kBone});
        if (bone.shape == RagdollBoneComponent::Shape::Capsule) {
            out.push_back({a, a - dir * r, kBone}); // cap tips
            out.push_back({b, b + dir * r, kBone});
        }
        out.push_back({self.position, self.position + dir * len, math::Color{1, 1, 1, 0.6f}}); // bone axis
    });
}

} // namespace detail

namespace {

// Lower-case name with separators removed: "mixamorig:LeftUpLeg" -> "mixamorigleftupleg".
std::string squash(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char ch : s)
        if (std::isalnum(static_cast<unsigned char>(ch))) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    return out;
}

enum class Side : std::uint8_t { None, Left, Right };

// Side from common conventions: Left/Right words, ".L"/"_l"/" L" suffixes, "L_"/"l_" prefixes.
Side side_of(std::string_view raw) {
    const std::string s = squash(raw);
    if (s.find("left") != std::string::npos) return Side::Left;
    if (s.find("right") != std::string::npos) return Side::Right;
    std::string lower(raw);
    for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    auto ends = [&](std::string_view suf) { return lower.size() > suf.size() && lower.ends_with(suf); };
    auto starts = [&](std::string_view pre) { return lower.size() > pre.size() && lower.starts_with(pre); };
    if (ends(".l") || ends("_l") || ends(" l") || ends("-l") || starts("l_") || starts("l.")) return Side::Left;
    if (ends(".r") || ends("_r") || ends(" r") || ends("-r") || starts("r_") || starts("r.")) return Side::Right;
    return Side::None;
}

enum class Role : std::uint8_t { None, Hips, Spine, Chest, Head, UpperArm, LowerArm, Hand, UpperLeg, LowerLeg, Foot };

Role role_of(std::string_view raw) {
    std::string s = squash(raw);
    for (const std::string_view prefix : {"mixamorig", "bip001", "bip01", "def", "armature"})
        if (s.starts_with(prefix)) s.erase(0, prefix.size());
    // Strip side words so "leftupleg" -> "upleg" and "upperarml" -> "upperarm".
    for (const std::string_view w : {"left", "right"})
        if (auto p = s.find(w); p != std::string::npos) s.erase(p, w.size());
    auto has = [&](std::string_view w) { return s.find(w) != std::string::npos; };
    if (has("finger") || has("thumb") || has("index") || has("toe") || has("twist") || has("end") || has("eye") ||
        has("jaw") || has("ball"))
        return Role::None;
    if (s.starts_with("ik") || has("pole") || has("target")) return Role::None; // IK helpers
    if (has("hips") || has("pelvis") || s == "hip") return Role::Hips;
    if (has("head")) return Role::Head;
    if (has("forearm") || has("lowerarm") || has("elbow")) return Role::LowerArm;
    if (has("hand") || has("wrist")) return Role::Hand;
    if (has("upleg") || has("upperleg") || has("thigh") || has("femur")) return Role::UpperLeg;
    if (has("lowerleg") || has("calf") || has("shin") || has("knee") || s == "leg" || s == "legl" || s == "legr")
        return Role::LowerLeg;
    if (has("foot") || has("ankle")) return Role::Foot;
    if (has("chest") || has("spine2") || has("spine3") || has("upperchest") || has("thorax")) return Role::Chest;
    if (has("spine") || has("abdomen") || has("waist")) return Role::Spine;
    if (has("arm")) return Role::UpperArm;
    return Role::None;
}

// Joint limits per role (degrees) and shape, after common game ragdoll setups (Unity Ragdoll Wizard,
// Jolt's sample human ragdoll): knees and elbows bend one way, shoulders and hips swing widely.
RagdollBoneComponent preset(Role role) {
    RagdollBoneComponent b;
    switch (role) {
    case Role::Hips: b.shape = RagdollBoneComponent::Shape::Box; b.radius = 0.13f; b.length = 0.18f; break;
    case Role::Spine: b.radius = 0.12f; b.swing = 20; b.swing_plane = 20; b.twist_min = -15; b.twist_max = 15; break;
    case Role::Chest: b.shape = RagdollBoneComponent::Shape::Box; b.radius = 0.14f; b.swing = 20; b.swing_plane = 20; b.twist_min = -20; b.twist_max = 20; break;
    case Role::Head: b.shape = RagdollBoneComponent::Shape::Sphere; b.radius = 0.11f; b.swing = 40; b.swing_plane = 40; b.twist_min = -60; b.twist_max = 60; break;
    case Role::UpperArm: b.radius = 0.05f; b.swing = 80; b.swing_plane = 80; b.twist_min = -60; b.twist_max = 60; break;
    case Role::LowerArm: b.radius = 0.045f; b.swing = 70; b.swing_plane = 5; b.twist_min = -60; b.twist_max = 60; break;
    case Role::Hand: b.shape = RagdollBoneComponent::Shape::Sphere; b.radius = 0.045f; b.swing = 60; b.swing_plane = 30; b.twist_min = -30; b.twist_max = 30; break;
    case Role::UpperLeg: b.radius = 0.075f; b.swing = 60; b.swing_plane = 45; b.twist_min = -30; b.twist_max = 30; break;
    case Role::LowerLeg: b.radius = 0.06f; b.swing = 70; b.swing_plane = 5; b.twist_min = -10; b.twist_max = 10; break;
    case Role::Foot: b.shape = RagdollBoneComponent::Shape::Box; b.radius = 0.05f; b.length = 0.16f; b.swing = 30; b.swing_plane = 15; b.twist_min = -10; b.twist_max = 10; break;
    case Role::None: break;
    }
    return b;
}

void collect(const scene::Scene& scene, Entity e, std::vector<std::pair<Entity, Role>>& out, int depth) {
    if (depth > 128) return;
    if (const auto* n = scene.world().get<scene::Name>(e)) {
        const Role r = role_of(n->value);
        if (r != Role::None) out.emplace_back(e, r);
    }
    for (const Entity c : scene.children(e)) collect(scene, c, out, depth + 1);
}

} // namespace

std::size_t make_humanoid_ragdoll(scene::Scene& scene, Entity root) {
    if (!scene.world().alive(root)) return 0;
    std::vector<std::pair<Entity, Role>> found;
    collect(scene, root, found, 0);
    // One bone per role and side; spine/chest keep the first (closest to the hips) and the last.
    std::vector<std::pair<Entity, Role>> chosen;
    bool hips = false;
    std::array<int, 3 * 11> taken{};
    for (const auto& [e, role] : found) {
        const Side side = (role == Role::Hips || role == Role::Spine || role == Role::Chest || role == Role::Head)
                              ? Side::None
                              : side_of(scene.world().get<scene::Name>(e)->value);
        if (side == Side::None && role != Role::Hips && role != Role::Spine && role != Role::Chest && role != Role::Head)
            continue; // a limb without a recognisable side
        int& count = taken[static_cast<std::size_t>(side) * 11 + static_cast<std::size_t>(role)];
        if (role == Role::Hips && hips) continue;
        if (count > 0 && role != Role::Chest) continue;
        if (role == Role::Chest && count > 0) {
            // Prefer the upper-most chest bone: replace the earlier pick.
            for (auto& c : chosen)
                if (c.second == Role::Chest) c.first = e;
            continue;
        }
        ++count;
        hips = hips || role == Role::Hips;
        chosen.emplace_back(e, role);
    }
    if (!hips) return 0;
    for (const auto& [e, role] : chosen) {
        RagdollBoneComponent b = preset(role);
        if (const auto* existing = scene.world().get<RagdollBoneComponent>(e)) b = *existing; // keep authored bones
        scene.world().add<RagdollBoneComponent>(e, b);
    }
    if (!scene.world().get<RagdollComponent>(root)) scene.world().add<RagdollComponent>(root, RagdollComponent{});
    return chosen.size();
}

} // namespace victus::physics
