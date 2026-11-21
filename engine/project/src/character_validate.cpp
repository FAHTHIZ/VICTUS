// SPDX-License-Identifier: Apache-2.0
// Character validation (docs/CHARACTERS.md#validating-a-character): checks that an imported model
// can replace a VICTUS starter character without script, controller or template changes.
#include "victus/project/characters.hpp"

#include "victus/assets/cooked.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <set>
#include <string>
#include <unordered_map>

namespace victus::project {

namespace {

struct CoreJoint {
    const char* name;
    const char* parent; // nullptr: the skeleton root (its parent is not a joint)
};

constexpr std::array<CoreJoint, kCharacterCoreJointCount> kCore = {{
    {"Hips", nullptr},           {"Spine", "Hips"},           {"Chest", "Spine"},          {"Neck", "Chest"},
    {"Head", "Neck"},            {"Shoulder.L", "Chest"},     {"UpperArm.L", "Shoulder.L"}, {"LowerArm.L", "UpperArm.L"},
    {"Hand.L", "LowerArm.L"},    {"Shoulder.R", "Chest"},     {"UpperArm.R", "Shoulder.R"}, {"LowerArm.R", "UpperArm.R"},
    {"Hand.R", "LowerArm.R"},    {"UpperLeg.L", "Hips"},      {"LowerLeg.L", "UpperLeg.L"}, {"Foot.L", "LowerLeg.L"},
    {"Toe.L", "Foot.L"},         {"UpperLeg.R", "Hips"},      {"LowerLeg.R", "UpperLeg.R"}, {"Foot.R", "LowerLeg.R"},
    {"Toe.R", "Foot.R"},
}};

struct SocketSpec {
    const char* name;
    const char* parent; // nullptr: any non-joint ancestor (the rig root)
};

constexpr std::array<SocketSpec, 8> kSockets = {{
    {"Socket.Root", nullptr},
    {"Socket.Head", "Head"},
    {"Socket.Face", "Head"},
    {"Socket.Back", "Chest"},
    {"Socket.Hand.L", "Hand.L"},
    {"Socket.Hand.R", "Hand.R"},
    {"Socket.Hip.L", "Hips"},
    {"Socket.Hip.R", "Hips"},
}};

void add(CharacterReport& r, CharacterIssue::Severity sev, std::string code, std::string message) {
    r.issues.push_back({sev, std::move(code), std::move(message)});
}

} // namespace

namespace {
template <typename Spec, std::size_t N>
constexpr std::array<const char*, N> spec_names(const std::array<Spec, N>& specs) {
    std::array<const char*, N> n{};
    for (std::size_t i = 0; i < N; ++i) n[i] = specs[i].name;
    return n;
}
constexpr auto kCoreNames = spec_names(kCore);
constexpr auto kSocketNames = spec_names(kSockets);
} // namespace

std::span<const char* const> character_core_joint_names() { return kCoreNames; }

std::span<const char* const> character_socket_names() { return kSocketNames; }

std::span<const char* const> character_controller_clip_names() {
    static constexpr std::array<const char*, 7> names = {"Idle", "Walk", "Run", "Jump", "Fall", "Land", "Interact"};
    return names;
}

bool CharacterReport::ok() const noexcept {
    return std::none_of(issues.begin(), issues.end(),
                        [](const CharacterIssue& i) { return i.severity == CharacterIssue::Severity::Error; });
}

std::size_t CharacterReport::count(CharacterIssue::Severity s) const noexcept {
    return static_cast<std::size_t>(
        std::count_if(issues.begin(), issues.end(), [s](const CharacterIssue& i) { return i.severity == s; }));
}

CharacterReport validate_character(const assets::CookedModel& m, const CharacterValidationOptions& o) {
    using Sev = CharacterIssue::Severity;
    CharacterReport r;
    r.triangles = m.triangle_count();
    r.clips = m.animations.size();
    r.materials = m.materials.size();
    r.textures = m.textures.size();

    if (const Status st = assets::validate_model(m); !st.ok()) add(r, Sev::Error, "model", st.error().message);

    // Skeleton: one skin whose first joints are the core layout with the core parents.
    if (m.skins.empty()) {
        add(r, Sev::Error, "skin", "the model has no skin; a character needs a skinned mesh bound to its skeleton");
        return r;
    }
    if (m.skins.size() > 1)
        add(r, Sev::Warning, "skin", std::format("{} skins; characters use one shared skeleton (the first is checked)", m.skins.size()));
    const auto& skin = m.skins.front();
    r.joints = static_cast<std::uint32_t>(skin.joints.size());
    std::unordered_map<std::string, std::uint32_t> node_of;
    for (std::uint32_t i = 0; i < m.nodes.size(); ++i) node_of.try_emplace(m.nodes[i].name, i);
    std::set<std::uint32_t> joint_nodes(skin.joints.begin(), skin.joints.end());
    auto parent_name = [&](std::uint32_t n) -> std::string {
        const auto p = m.nodes[n].parent;
        return p < 0 ? std::string() : m.nodes[static_cast<std::size_t>(p)].name;
    };
    if (skin.joints.size() < kCharacterCoreJointCount)
        add(r, Sev::Error, "joints",
            std::format("{} joints; the humanoid layout needs at least {}", skin.joints.size(), kCharacterCoreJointCount));
    for (std::size_t j = 0; j < kCore.size(); ++j) {
        const auto it = node_of.find(kCore[j].name);
        if (it == node_of.end() || !joint_nodes.count(it->second)) {
            add(r, Sev::Error, "joints", std::format("core joint '{}' is missing from the skin", kCore[j].name));
            continue;
        }
        if (j < skin.joints.size() && skin.joints[j] != it->second)
            add(r, Sev::Error, "joints",
                std::format("core joint '{}' is skin joint {} but must be joint {} (the original 21-joint order comes "
                            "first so scripts and retarget maps keep working)",
                            kCore[j].name,
                            std::distance(skin.joints.begin(), std::find(skin.joints.begin(), skin.joints.end(), it->second)), j));
        const std::string p = parent_name(it->second);
        if (kCore[j].parent ? p != kCore[j].parent : (m.nodes[it->second].parent >= 0 &&
                                                       joint_nodes.count(static_cast<std::uint32_t>(m.nodes[it->second].parent))))
            add(r, Sev::Error, "joints",
                std::format("core joint '{}' has parent '{}', expected '{}'", kCore[j].name, p,
                            kCore[j].parent ? kCore[j].parent : "a non-joint rig node"));
    }
    if (o.expect_joint_count && skin.joints.size() != o.expect_joint_count)
        add(r, Sev::Warning, "joints",
            std::format("{} joints; the VICTUS skeleton has {} (extra or missing finger, twist, jaw or eye joints)",
                        skin.joints.size(), o.expect_joint_count));

    // Sockets: attachment points equipment and scripts look up by name.
    for (const auto& s : kSockets) {
        const auto it = node_of.find(s.name);
        if (it == node_of.end()) {
            add(r, o.require_sockets ? Sev::Error : Sev::Warning, "sockets", std::format("socket '{}' is missing", s.name));
            continue;
        }
        ++r.sockets;
        const std::string p = parent_name(it->second);
        if (s.parent && p != s.parent)
            add(r, Sev::Warning, "sockets", std::format("socket '{}' is under '{}', expected '{}'", s.name, p, s.parent));
    }

    // Clips the shared controller plays.
    for (const char* c : character_controller_clip_names()) {
        const auto it = std::find_if(m.animations.begin(), m.animations.end(), [&](const auto& a) { return a.name == c; });
        if (it == m.animations.end())
            add(r, o.require_controller_clips ? Sev::Error : Sev::Warning, "clips",
                std::format("clip '{}' (used by the shared locomotion controller) is missing", c));
        else if (!(it->duration > 0.0f))
            add(r, Sev::Error, "clips", std::format("clip '{}' has no length", c));
    }

    // Meshes: skinned, weights normalised and in range, LODs, bind-pose height.
    float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
    std::size_t skinned = 0, bad_weights = 0, bad_index = 0;
    for (const auto& node : m.nodes) {
        for (const auto mi : node.meshes) {
            if (mi >= m.meshes.size()) continue;
            const auto& mesh = m.meshes[mi].data;
            r.morphs = std::max(r.morphs, mesh.morphs.size());
            r.lods = std::max(r.lods, static_cast<std::uint32_t>(mesh.lods.size()));
            for (const auto& v : mesh.vertices) {
                lo = std::min(lo, v.position.y);
                hi = std::max(hi, v.position.y);
            }
            if (mesh.skin.empty() || node.skin < 0) {
                add(r, Sev::Warning, "mesh", std::format("mesh '{}' is not skinned and will not deform", m.meshes[mi].name));
                continue;
            }
            ++skinned;
            for (const auto& sv : mesh.skin) {
                float sum = 0;
                for (int k = 0; k < 4; ++k) {
                    sum += sv.weights[k];
                    if (sv.weights[k] > 0 && sv.joints[k] >= skin.joints.size()) ++bad_index;
                }
                if (std::abs(sum - 1.0f) > 0.01f) ++bad_weights;
            }
        }
    }
    if (skinned == 0) add(r, Sev::Error, "mesh", "no skinned mesh is bound to the skeleton");
    if (bad_index) add(r, Sev::Error, "mesh", std::format("{} vertex weights reference joints outside the skin", bad_index));
    if (bad_weights)
        add(r, Sev::Warning, "mesh", std::format("{} vertices have skin weights that do not sum to 1", bad_weights));
    if (lo <= hi) {
        r.height = hi - lo;
        if (std::abs(lo) > 0.05f)
            add(r, Sev::Warning, "scale", std::format("the bind pose starts {:.3f} m from the ground; characters stand at y = 0", lo));
        if (r.height < o.min_height || r.height > o.max_height)
            add(r, Sev::Error, "scale",
                std::format("bind-pose height {:.3f} m is outside {:.2f}-{:.2f} m (wrong units or axis?)", r.height,
                            o.min_height, o.max_height));
    }
    if (r.lods == 0 && r.triangles > o.lod_triangle_threshold)
        add(r, Sev::Warning, "lod",
            std::format("{} triangles and no LODs; import with LOD generation or author MSFT_lod levels", r.triangles));

    // Materials: textured PBR rather than flat colours.
    for (const auto& mat : m.materials) {
        if (mat.base_color_texture < 0)
            add(r, Sev::Warning, "materials", std::format("material '{}' has no base colour texture", mat.name));
        if (mat.normal_texture < 0)
            add(r, Sev::Warning, "materials", std::format("material '{}' has no normal map", mat.name));
    }
    return r;
}

} // namespace victus::project
