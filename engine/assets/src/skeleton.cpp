// SPDX-License-Identifier: Apache-2.0
// Skeleton inspection, extension, migration and compatibility checks (skeleton.hpp, ADR-0205).
#include "victus/assets/skeleton.hpp"
#include "victus/assets/importers.hpp"

#include "victus/animation/animation.hpp"
#include "victus/render/skinning.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <unordered_map>

namespace victus::assets {

using math::Mat4;
using math::Quat;
using math::Vec3;

// ---- roles -------------------------------------------------------------------------------------

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool has(std::string_view hay, std::string_view needle) { return hay.find(needle) != std::string_view::npos; }
bool starts(std::string_view hay, std::string_view prefix) { return hay.substr(0, prefix.size()) == prefix; }

constexpr std::array<std::pair<std::uint32_t, const char*>, 24> kRoleNames = {{
    {kBoneRoot, "root"},         {kBoneSpine, "spine"},       {kBoneNeck, "neck"},         {kBoneHead, "head"},
    {kBoneArm, "arm"},           {kBoneHand, "hand"},         {kBoneFinger, "finger"},     {kBoneLeg, "leg"},
    {kBoneFoot, "foot"},         {kBoneToe, "toe"},           {kBoneTwist, "twist"},       {kBoneFace, "face"},
    {kBoneEye, "eye"},           {kBoneJaw, "jaw"},           {kBoneTongue, "tongue"},     {kBoneSecondary, "secondary"},
    {kBoneMechanical, "mechanical"}, {kBoneCorrective, "corrective"}, {kBoneIkTarget, "ik_target"}, {kBoneIkPole, "ik_pole"},
    {kBoneSocket, "socket"},     {kBoneTail, "tail"},         {kBoneWing, "wing"},         {kBoneEar, "ear"},
}};

} // namespace

std::uint32_t infer_bone_roles(std::string_view name) noexcept {
    const std::string n = lower(name);
    std::uint32_t r = 0;
    if (starts(n, "socket") || starts(n, "attach") || has(n, "_socket") || starts(n, "equip") || starts(n, "mount")) r |= kBoneSocket;
    if (starts(n, "ik.") || starts(n, "ik_") || has(n, "ik_target") || starts(n, "ik-") || has(n, "_ik") || has(n, ".ik"))
        r |= kBoneIkTarget;
    if (starts(n, "pole") || has(n, "_pole") || has(n, ".pole")) r |= kBoneIkPole;
    if (starts(n, "mech.") || has(n, "piston") || has(n, "hinge") || has(n, "wheel") || has(n, "turret") || has(n, "gear") ||
        has(n, "servo") || has(n, "rotor"))
        r |= kBoneMechanical;
    if (starts(n, "dyn.") || has(n, "jiggle") || has(n, "spring") || has(n, "cloth") || has(n, "hair") || has(n, "antenna") ||
        has(n, "cable") || has(n, "breast") || has(n, "belly"))
        r |= kBoneSecondary;
    if (starts(n, "fix.") || has(n, "corrective") || has(n, "helper") || has(n, "volume")) r |= kBoneCorrective;
    if (has(n, "twist") || has(n, "roll")) r |= kBoneTwist;
    if (n == "root" || n == "rig" || n == "armature" || starts(n, "root") || n == "master") r |= kBoneRoot;
    if (has(n, "hips") || has(n, "pelvis") || has(n, "spine") || has(n, "chest") || has(n, "torso") || has(n, "abdomen"))
        r |= kBoneSpine;
    if (has(n, "neck")) r |= kBoneNeck;
    if (has(n, "head") && !has(n, "forehead")) r |= kBoneHead;
    if (has(n, "eye") && !has(n, "eyebrow") && !has(n, "eyelid")) r |= kBoneEye;
    if (has(n, "jaw") || has(n, "chin")) r |= kBoneJaw;
    if (has(n, "tongue")) r |= kBoneTongue;
    if (has(n, "brow") || has(n, "lid") || has(n, "lip") || has(n, "cheek") || has(n, "nose") || has(n, "mouth") ||
        has(n, "forehead") || has(n, "nostril") || has(n, "face") || has(n, "teeth") || has(n, "eyebrow"))
        r |= kBoneFace;
    if (has(n, "ear")) r |= kBoneEar;
    if (has(n, "tail")) r |= kBoneTail;
    if (has(n, "wing") || has(n, "feather")) r |= kBoneWing;
    if (has(n, "thumb") || has(n, "index") || has(n, "middle") || has(n, "ring") || has(n, "pinky") || has(n, "finger") ||
        has(n, "claw") || has(n, "digit"))
        r |= kBoneFinger;
    if (has(n, "toe") || has(n, "hallux")) r |= kBoneToe;
    if ((has(n, "hand") || has(n, "wrist") || has(n, "palm")) && !(r & kBoneFinger)) r |= kBoneHand;
    if (has(n, "shoulder") || has(n, "clavicle") || has(n, "upperarm") || has(n, "lowerarm") || has(n, "forearm") ||
        has(n, "elbow") || (has(n, "arm") && !has(n, "armature")))
        r |= kBoneArm;
    if (has(n, "thigh") || has(n, "upperleg") || has(n, "lowerleg") || has(n, "calf") || has(n, "shin") || has(n, "knee") ||
        has(n, "leg") || has(n, "hock"))
        r |= kBoneLeg;
    if ((has(n, "foot") || has(n, "ankle") || has(n, "paw") || has(n, "hoof")) && !(r & kBoneToe)) r |= kBoneFoot;
    // Mid-limb names that fingers would also match ("middle" contains "middle", "ring" in "string").
    if ((r & kBoneFinger) && (r & kBoneSpine)) r &= ~kBoneFinger;
    return r;
}

std::string bone_roles_string(std::uint32_t roles) {
    std::string out;
    for (const auto& [bit, name] : kRoleNames)
        if (roles & bit) {
            if (!out.empty()) out += '|';
            out += name;
        }
    return out;
}

std::uint32_t parse_bone_roles(std::string_view text) noexcept {
    std::uint32_t r = 0;
    while (!text.empty()) {
        const std::size_t bar = text.find('|');
        const std::string_view tok = text.substr(0, bar);
        for (const auto& [bit, name] : kRoleNames)
            if (tok == name) r |= bit;
        if (bar == std::string_view::npos) break;
        text.remove_prefix(bar + 1);
    }
    return r;
}

// ---- helpers -----------------------------------------------------------------------------------

namespace {

Mat4 local_matrix(const ModelNode& n) { return Mat4::trs(n.translation, n.rotation, n.scale); }

std::vector<Mat4> rest_world(const CookedModel& m) {
    std::vector<Mat4> w(m.nodes.size());
    for (std::size_t i = 0; i < m.nodes.size(); ++i) {
        const ModelNode& n = m.nodes[i];
        w[i] = n.parent >= 0 && static_cast<std::size_t>(n.parent) < i ? w[static_cast<std::size_t>(n.parent)] * local_matrix(n)
                                                                         : local_matrix(n);
    }
    return w;
}

std::int32_t find_node(const CookedModel& m, std::string_view name) {
    for (std::size_t i = 0; i < m.nodes.size(); ++i)
        if (m.nodes[i].name == name) return static_cast<std::int32_t>(i);
    return -1;
}

std::int32_t joint_of(const ModelSkin& s, std::int32_t node) {
    for (std::size_t j = 0; j < s.joints.size(); ++j)
        if (static_cast<std::int32_t>(s.joints[j]) == node) return static_cast<std::int32_t>(j);
    return -1;
}

bool is_descendant(const CookedModel& m, std::int32_t node, std::int32_t ancestor) {
    for (std::int32_t p = node; p >= 0; p = m.nodes[static_cast<std::size_t>(p)].parent)
        if (p == ancestor) return true;
    return false;
}

// Meshes whose skin streams index into `skin`.
std::vector<std::uint32_t> skin_meshes(const CookedModel& m, std::uint32_t skin, bool require_stream = true) {
    std::set<std::uint32_t> out;
    for (const ModelNode& n : m.nodes)
        if (n.skin == static_cast<std::int32_t>(skin))
            for (const std::uint32_t mi : n.meshes)
                if (mi < m.meshes.size() && (!require_stream || m.meshes[mi].data.skinned())) out.insert(mi);
    return {out.begin(), out.end()};
}

Vec3 position(const Mat4& m) { return m.c[3].xyz(); }

float smoothstep(float a, float b, float x) {
    if (b <= a) return x >= b ? 1.0f : 0.0f;
    const float t = math::saturate((x - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}

// Adds `w` of `joint` to a skin vertex (merging with an existing slot), keeping the strongest four.
render::SkinVertex add_influence(const render::SkinVertex& v, std::uint32_t joint, float w, std::uint32_t from, float take) {
    std::uint32_t j[5];
    float ws[5];
    std::size_t n = 0;
    bool merged = false;
    for (int k = 0; k < 4; ++k) {
        if (!(v.weights[k] > 0.0f)) continue;
        j[n] = v.joints[k];
        ws[n] = v.weights[k] - (v.joints[k] == from ? take : 0.0f);
        if (v.joints[k] == joint) {
            ws[n] += w;
            merged = true;
        }
        ++n;
    }
    if (!merged) {
        j[n] = joint;
        ws[n++] = w;
    }
    for (std::size_t k = 0; k < n; ++k) ws[k] = std::max(ws[k], 0.0f);
    return render::make_skin_vertex(j, ws, n);
}

std::vector<Mat4> clip_world(const CookedModel& m, const animation::Clip& clip, float t) { return pose_world(m, &clip, t); }

std::set<std::uint32_t> animated_nodes(const animation::Clip& c) {
    std::set<std::uint32_t> s;
    for (const auto& ch : c.channels)
        if (ch.path != animation::Path::Weights) s.insert(ch.node);
    return s;
}

// Re-bakes `bone`'s transform channels in every clip so its world motion stays the same once
// its parent becomes `new_parent` (-1: root). Runs before the parent changes. Returns channels written.
std::size_t rebake_for_reparent(CookedModel& m, std::int32_t bone, std::int32_t new_parent, const SkeletonEditOptions& o,
                                std::vector<std::string>& warnings) {
    std::size_t written = 0;
    const std::int32_t old_parent = m.nodes[static_cast<std::size_t>(bone)].parent;
    for (animation::Clip& clip : m.animations) {
        const auto animated = animated_nodes(clip);
        const auto chain_animated = [&](std::int32_t from) {
            for (std::int32_t p = from; p >= 0; p = m.nodes[static_cast<std::size_t>(p)].parent)
                if (animated.count(static_cast<std::uint32_t>(p))) return true;
            return false;
        };
        if (!animated.count(static_cast<std::uint32_t>(bone)) && !chain_animated(old_parent) && !chain_animated(new_parent))
            continue;
        // Sample at every key of the involved channels plus a uniform rate between them.
        std::set<float> times{0.0f, clip.duration};
        for (const auto& ch : clip.channels)
            for (const float t : ch.times) times.insert(t);
        if (o.bake_rate > 0 && clip.duration > 0) {
            const auto n = static_cast<std::uint32_t>(std::ceil(clip.duration * o.bake_rate));
            for (std::uint32_t i = 0; i <= n; ++i) times.insert(clip.duration * static_cast<float>(i) / static_cast<float>(std::max(n, 1u)));
        }
        std::vector<float> ts(times.begin(), times.end());
        if (ts.size() > o.max_bake_samples) {
            warnings.push_back(std::format("clip '{}': re-baked '{}' with {} of {} samples", clip.name,
                                           m.nodes[static_cast<std::size_t>(bone)].name, o.max_bake_samples, ts.size()));
            std::vector<float> keep;
            for (std::uint32_t i = 0; i < o.max_bake_samples; ++i)
                keep.push_back(ts[static_cast<std::size_t>(std::llround(static_cast<double>(i) * static_cast<double>(ts.size() - 1) /
                                                                       static_cast<double>(o.max_bake_samples - 1)))]);
            ts = std::move(keep);
        }
        const auto make_channel = [&](animation::Path path) {
            animation::Channel ch;
            ch.node = static_cast<std::uint32_t>(bone);
            ch.path = path;
            ch.interpolation = animation::Interpolation::Linear;
            ch.times = ts;
            return ch;
        };
        animation::Channel tr = make_channel(animation::Path::Translation);
        animation::Channel ro = make_channel(animation::Path::Rotation);
        animation::Channel sc = make_channel(animation::Path::Scale);
        Quat prev{};
        for (const float t : ts) {
            const std::vector<Mat4> w = clip_world(m, clip, t);
            const Mat4 parent_w = new_parent >= 0 ? w[static_cast<std::size_t>(new_parent)] : Mat4{};
            const Mat4 local = math::inverse(parent_w) * w[static_cast<std::size_t>(bone)];
            Vec3 pt, ps;
            Quat pr;
            math::decompose(local, pt, pr, ps);
            if (pr.x * prev.x + pr.y * prev.y + pr.z * prev.z + pr.w * prev.w < 0) pr = {-pr.x, -pr.y, -pr.z, -pr.w};
            prev = pr;
            tr.values.insert(tr.values.end(), {pt.x, pt.y, pt.z});
            ro.values.insert(ro.values.end(), {pr.x, pr.y, pr.z, pr.w});
            sc.values.insert(sc.values.end(), {ps.x, ps.y, ps.z});
        }
        std::erase_if(clip.channels, [&](const animation::Channel& ch) {
            return ch.node == static_cast<std::uint32_t>(bone) && ch.path != animation::Path::Weights;
        });
        animation::KeyframeReduction red;
        for (auto* ch : {&tr, &ro, &sc}) {
            (void)animation::reduce_keyframes(*ch, red);
            clip.channels.push_back(std::move(*ch));
            ++written;
        }
    }
    return written;
}

// Reorders nodes so parents precede children (stable) and remaps every node reference.
std::vector<std::int32_t> sort_nodes(CookedModel& m) {
    const std::size_t n = m.nodes.size();
    std::vector<std::int32_t> order;
    order.reserve(n);
    std::vector<std::uint8_t> placed(n, 0);
    std::vector<std::vector<std::int32_t>> kids(n);
    std::vector<std::int32_t> roots;
    for (std::size_t i = 0; i < n; ++i) {
        const std::int32_t p = m.nodes[i].parent;
        if (p >= 0) kids[static_cast<std::size_t>(p)].push_back(static_cast<std::int32_t>(i));
        else roots.push_back(static_cast<std::int32_t>(i));
    }
    // Keep existing order where valid: emit nodes in index order, each only once its parent is.
    bool progress = true;
    while (order.size() < n && progress) {
        progress = false;
        for (std::size_t i = 0; i < n; ++i) {
            if (placed[i]) continue;
            const std::int32_t p = m.nodes[i].parent;
            if (p < 0 || placed[static_cast<std::size_t>(p)]) {
                placed[i] = 1;
                order.push_back(static_cast<std::int32_t>(i));
                progress = true;
            }
        }
    }
    std::vector<std::int32_t> remap(n, -1);
    for (std::size_t k = 0; k < order.size(); ++k) remap[static_cast<std::size_t>(order[k])] = static_cast<std::int32_t>(k);
    std::vector<ModelNode> nodes;
    nodes.reserve(n);
    for (const std::int32_t i : order) nodes.push_back(std::move(m.nodes[static_cast<std::size_t>(i)]));
    for (ModelNode& nd : nodes)
        if (nd.parent >= 0) nd.parent = remap[static_cast<std::size_t>(nd.parent)];
    m.nodes = std::move(nodes);
    for (ModelSkin& s : m.skins)
        for (std::uint32_t& j : s.joints) j = static_cast<std::uint32_t>(remap[j]);
    for (animation::Clip& c : m.animations)
        for (animation::Channel& ch : c.channels) ch.node = static_cast<std::uint32_t>(remap[ch.node]);
    return remap;
}

std::vector<std::int32_t> identity_map(std::size_t n) {
    std::vector<std::int32_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::int32_t>(i);
    return v;
}

void compose_map(std::vector<std::int32_t>& total, const std::vector<std::int32_t>& step) {
    for (std::int32_t& v : total)
        if (v >= 0) v = static_cast<std::size_t>(v) < step.size() ? step[static_cast<std::size_t>(v)] : -1;
}

bool is_protected(const SkeletonEditOptions& o, std::string_view name) {
    return std::find(o.protected_bones.begin(), o.protected_bones.end(), name) != o.protected_bones.end();
}

Status reparent_node(CookedModel& m, std::int32_t bone, std::int32_t new_parent, const SkeletonEditOptions& o,
                     SkeletonEditReport& r) {
    if (new_parent >= 0 && is_descendant(m, new_parent, bone))
        return make_error(ErrorCode::InvalidArgument,
                          std::format("cannot parent '{}' under its own descendant '{}'", m.nodes[static_cast<std::size_t>(bone)].name,
                                      m.nodes[static_cast<std::size_t>(new_parent)].name));
    r.channels_rebaked += rebake_for_reparent(m, bone, new_parent, o, r.warnings);
    const std::vector<Mat4> w = rest_world(m);
    const Mat4 pw = new_parent >= 0 ? w[static_cast<std::size_t>(new_parent)] : Mat4{};
    ModelNode& node = m.nodes[static_cast<std::size_t>(bone)];
    math::decompose(math::inverse(pw) * w[static_cast<std::size_t>(bone)], node.translation, node.rotation, node.scale);
    node.parent = new_parent;
    if (new_parent > bone) compose_map(r.node_remap, sort_nodes(m));
    return {};
}

} // namespace

// ---- posing ------------------------------------------------------------------------------------

std::vector<Mat4> pose_world(const CookedModel& m, const animation::Clip* clip, float t) { return pose_world(m, clip, t, {}); }

std::vector<Mat4> pose_world(const CookedModel& m, const animation::Clip* clip, float t,
                             std::span<const std::pair<std::uint32_t, Quat>> overrides) {
    std::vector<animation::NodePose> pose(m.nodes.size());
    for (std::size_t i = 0; i < m.nodes.size(); ++i) pose[i] = {m.nodes[i].translation, m.nodes[i].rotation, m.nodes[i].scale};
    if (clip) animation::sample(*clip, t, pose);
    for (const auto& [node, q] : overrides)
        if (node < pose.size()) pose[node].rotation = math::normalize(q);
    std::vector<Mat4> w(m.nodes.size());
    for (std::size_t i = 0; i < m.nodes.size(); ++i) {
        const Mat4 l = Mat4::trs(pose[i].translation, pose[i].rotation, pose[i].scale);
        const std::int32_t p = m.nodes[i].parent;
        w[i] = p >= 0 && static_cast<std::size_t>(p) < i ? w[static_cast<std::size_t>(p)] * l : l;
    }
    return w;
}

std::vector<Mat4> pose_palette(const CookedModel& m, std::span<const Mat4> world, std::uint32_t skin) {
    std::vector<Mat4> pal;
    if (skin >= m.skins.size()) return pal;
    const ModelSkin& s = m.skins[skin];
    pal.resize(s.joints.size());
    for (std::size_t j = 0; j < s.joints.size(); ++j)
        pal[j] = s.joints[j] < world.size() && j < s.inverse_bind.size() ? world[s.joints[j]] * s.inverse_bind[j] : Mat4{};
    return pal;
}

// ---- inspection --------------------------------------------------------------------------------

std::vector<Mat4> bind_transforms(const CookedModel& model, std::uint32_t skin) {
    std::vector<Mat4> w = rest_world(model);
    if (skin < model.skins.size()) {
        const ModelSkin& s = model.skins[skin];
        for (std::size_t j = 0; j < s.joints.size() && j < s.inverse_bind.size(); ++j)
            if (s.joints[j] < w.size()) w[s.joints[j]] = math::inverse(s.inverse_bind[j]);
    }
    // Non-joint nodes below joints follow their parent's bind transform.
    std::vector<std::uint8_t> is_joint(model.nodes.size(), 0);
    if (skin < model.skins.size())
        for (const std::uint32_t j : model.skins[skin].joints)
            if (j < is_joint.size()) is_joint[j] = 1;
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        if (is_joint[i]) continue;
        const std::int32_t p = model.nodes[i].parent;
        if (p >= 0) w[i] = w[static_cast<std::size_t>(p)] * local_matrix(model.nodes[i]);
    }
    return w;
}

std::uint64_t skeleton_layout_hash(const CookedModel& model, std::uint32_t skin) {
    if (skin >= model.skins.size()) return 0;
    std::uint64_t h = fnv1a64("victus.skeleton.v1");
    for (const std::uint32_t j : model.skins[skin].joints) {
        if (j >= model.nodes.size()) continue;
        const ModelNode& n = model.nodes[j];
        h = fnv1a64(n.name, h);
        h = fnv1a64("\x1f", h);
        h = fnv1a64(n.parent >= 0 ? std::string_view(model.nodes[static_cast<std::size_t>(n.parent)].name) : std::string_view("-"), h);
        h = fnv1a64("\x1e", h);
    }
    return h;
}

const SkeletonBone* SkeletonInfo::find(std::string_view name) const noexcept {
    for (const auto& b : bones)
        if (b.name == name) return &b;
    return nullptr;
}

std::uint32_t SkeletonInfo::count(std::uint32_t role) const noexcept {
    return static_cast<std::uint32_t>(std::count_if(bones.begin(), bones.end(), [&](const SkeletonBone& b) { return (b.roles & role) != 0; }));
}

SkeletonInfo describe_skeleton(const CookedModel& model, std::uint32_t skin) {
    SkeletonInfo info;
    info.skin = skin;
    if (skin >= model.skins.size()) return info;
    const ModelSkin& s = model.skins[skin];
    const std::size_t n = model.nodes.size();
    // A node is reported under its first skin slot; later slots that alias it (kept slots of
    // removed bones) add their weighted vertices to it.
    std::vector<std::int32_t> joint(n, -1);
    for (std::size_t j = 0; j < s.joints.size(); ++j)
        if (s.joints[j] < n && joint[s.joints[j]] < 0) joint[s.joints[j]] = static_cast<std::int32_t>(j);
    // Skeleton nodes: joints, their mesh-less descendants and their mesh-less ancestors.
    std::vector<std::uint8_t> in(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        if (joint[i] >= 0) {
            in[i] = 1;
            for (std::int32_t p = model.nodes[i].parent; p >= 0; p = model.nodes[static_cast<std::size_t>(p)].parent)
                if (model.nodes[static_cast<std::size_t>(p)].meshes.empty()) in[static_cast<std::size_t>(p)] = 1;
        }
    }
    for (bool grew = true; grew;) { // mesh-less descendants, whatever the node order
        grew = false;
        for (std::size_t i = 0; i < n; ++i)
            if (!in[i] && model.nodes[i].parent >= 0 && in[static_cast<std::size_t>(model.nodes[i].parent)] &&
                model.nodes[i].meshes.empty() && model.nodes[i].light < 0 && model.nodes[i].camera < 0) {
                in[i] = 1;
                grew = true;
            }
    }
    std::vector<std::uint32_t> weighted(s.joints.size(), 0);
    for (const std::uint32_t mi : skin_meshes(model, skin)) {
        const render::SkinWeightReport wr = render::analyze_skin_weights(model.meshes[mi].data, s.joints.size());
        for (std::size_t j = 0; j < weighted.size(); ++j) weighted[j] += wr.joint_vertices[j];
    }
    for (std::size_t j = 0; j < s.joints.size(); ++j)
        if (s.joints[j] < n && joint[s.joints[j]] != static_cast<std::int32_t>(j)) {
            weighted[static_cast<std::size_t>(joint[s.joints[j]])] += weighted[j];
            weighted[j] = 0;
        }
    std::set<std::uint32_t> animated;
    for (const auto& c : model.animations)
        for (const auto& ch : c.channels) animated.insert(ch.node);
    const std::vector<Mat4> bind = bind_transforms(model, skin);
    std::vector<std::uint32_t> depth(n, 0), children(n, 0);
    std::vector<std::int32_t> first_child(n, -1);
    for (std::size_t i = 0; i < n; ++i) {
        const std::int32_t p = model.nodes[i].parent;
        if (p < 0) continue;
        depth[i] = depth[static_cast<std::size_t>(p)] + 1;
        if (!in[i]) continue;
        ++children[static_cast<std::size_t>(p)];
        if (first_child[static_cast<std::size_t>(p)] < 0 && (joint[i] >= 0 || joint[static_cast<std::size_t>(p)] < 0))
            first_child[static_cast<std::size_t>(p)] = static_cast<std::int32_t>(i);
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!in[i]) continue;
        SkeletonBone b;
        b.name = model.nodes[i].name;
        b.node = static_cast<std::int32_t>(i);
        b.parent_node = model.nodes[i].parent;
        b.joint = joint[i];
        b.roles = infer_bone_roles(b.name);
        b.depth = depth[i];
        b.children = children[i];
        b.weighted_vertices = joint[i] >= 0 ? weighted[static_cast<std::size_t>(joint[i])] : 0;
        b.rest_position = position(bind[i]);
        if (first_child[i] >= 0) b.length = math::length(position(bind[static_cast<std::size_t>(first_child[i])]) - b.rest_position);
        b.animated = animated.count(static_cast<std::uint32_t>(i)) > 0;
        info.max_depth = std::max(info.max_depth, b.depth);
        if (b.joint >= 0) {
            ++info.joints;
            if (b.weighted_vertices == 0) ++info.unweighted_joints;
        } else if (b.roles & kBoneSocket) {
            ++info.sockets;
        } else {
            ++info.helpers;
        }
        info.bones.push_back(std::move(b));
    }
    info.layout_hash = skeleton_layout_hash(model, skin);
    return info;
}

// ---- editing -----------------------------------------------------------------------------------

Result<SkeletonEditReport> apply_skeleton_edits(CookedModel& model, std::span<const BoneEdit> edits, const SkeletonEditOptions& o) {
    if (o.skin >= model.skins.size()) return make_error(ErrorCode::InvalidArgument, std::format("model has no skin {}", o.skin));
    CookedModel m = model; // edits are transactional
    SkeletonEditReport r;
    r.layout_before = skeleton_layout_hash(m, o.skin);
    r.node_remap = identity_map(m.nodes.size());
    r.joint_remap = identity_map(m.skins[o.skin].joints.size());
    const auto fail = [](std::string msg) { return make_error(ErrorCode::InvalidArgument, std::move(msg)); };
    for (const BoneEdit& e : edits) {
        ModelSkin& skin = m.skins[o.skin];
        const std::int32_t node = find_node(m, e.bone);
        switch (e.op) {
        case BoneEdit::Op::Add: {
            if (e.bone.empty()) return fail("add: empty bone name");
            if (node >= 0) return fail(std::format("add: bone '{}' already exists", e.bone));
            const std::int32_t parent = find_node(m, e.parent);
            if (parent < 0) return fail(std::format("add '{}': parent '{}' not found", e.bone, e.parent));
            if (!std::isfinite(e.translation.x + e.translation.y + e.translation.z + e.rotation.x + e.rotation.y + e.rotation.z +
                               e.rotation.w + e.scale.x + e.scale.y + e.scale.z))
                return fail(std::format("add '{}': non-finite rest transform", e.bone));
            ModelNode nn;
            nn.name = e.bone;
            nn.parent = parent;
            nn.translation = e.translation;
            nn.rotation = math::normalize(e.rotation);
            nn.scale = e.scale;
            const std::vector<Mat4> bind = bind_transforms(m, o.skin);
            const Mat4 new_bind = bind[static_cast<std::size_t>(parent)] * local_matrix(nn);
            m.nodes.push_back(std::move(nn));
            const auto new_node = static_cast<std::uint32_t>(m.nodes.size() - 1);
            r.added.push_back(e.bone);
            if (!e.deform) {
                if (!e.weights_from.empty()) r.warnings.push_back(std::format("add '{}': weights_from ignored on a non-deforming bone", e.bone));
                break;
            }
            if (skin.joints.size() >= 65535) return fail("add: skin joint limit reached");
            skin.joints.push_back(new_node);
            skin.inverse_bind.push_back(math::inverse(new_bind));
            const auto new_joint = static_cast<std::uint32_t>(skin.joints.size() - 1);
            if (e.weights_from.empty() || !(e.weight_share > 0.0f)) break;
            const std::int32_t src_node = find_node(m, e.weights_from);
            const std::int32_t src = src_node >= 0 ? joint_of(skin, src_node) : -1;
            if (src < 0) return fail(std::format("add '{}': weights_from '{}' is not a joint of the skin", e.bone, e.weights_from));
            // Source bone segment: its head to its first child joint (or to the new bone).
            const std::vector<Mat4> b2 = bind_transforms(m, o.skin);
            const Vec3 head = position(b2[static_cast<std::size_t>(src_node)]);
            Vec3 tail = position(new_bind);
            for (std::size_t i = 0; i < m.nodes.size(); ++i)
                if (m.nodes[i].parent == src_node && joint_of(skin, static_cast<std::int32_t>(i)) >= 0 && i != new_node) {
                    tail = position(b2[i]);
                    break;
                }
            const Vec3 axis = tail - head;
            const float len2 = math::dot(axis, axis);
            for (const std::uint32_t mi : skin_meshes(m, o.skin)) {
                render::MeshData& d = m.meshes[mi].data;
                for (std::size_t vi = 0; vi < d.skin.size(); ++vi) {
                    render::SkinVertex& sv = d.skin[vi];
                    float ws = 0;
                    for (int k = 0; k < 4; ++k)
                        if (sv.joints[k] == static_cast<std::uint16_t>(src)) ws += sv.weights[k];
                    if (!(ws > 0.0f)) continue;
                    const float t = len2 > 1e-12f ? math::dot(d.vertices[vi].position - head, axis) / len2 : 1.0f;
                    const float take = ws * math::saturate(e.weight_share) * smoothstep(e.weight_start, e.weight_end, t);
                    if (!(take > 1e-4f)) continue;
                    // A vertex with four influences already would lose one, changing how it
                    // deforms under existing poses; it keeps its weights and is reported.
                    int used = 0;
                    for (int k = 0; k < 4; ++k) used += sv.weights[k] > 0.0f ? 1 : 0;
                    if (used >= 4 && take < ws) {
                        ++r.vertices_full;
                        continue;
                    }
                    sv = add_influence(sv, new_joint, take, static_cast<std::uint32_t>(src), take);
                    ++r.vertices_reweighted;
                }
            }
            break;
        }
        case BoneEdit::Op::Rename: {
            if (node < 0) return fail(std::format("rename: bone '{}' not found", e.bone));
            if (e.name.empty() || find_node(m, e.name) >= 0) return fail(std::format("rename '{}': '{}' is empty or taken", e.bone, e.name));
            if (is_protected(o, e.bone) && !e.force)
                return fail(std::format("rename: '{}' is protected (scripts, sockets or the shared controller use it); set force", e.bone));
            m.nodes[static_cast<std::size_t>(node)].name = e.name;
            r.renamed.emplace_back(e.bone, e.name);
            break;
        }
        case BoneEdit::Op::Reparent: {
            if (node < 0) return fail(std::format("reparent: bone '{}' not found", e.bone));
            const std::int32_t parent = e.parent.empty() ? -1 : find_node(m, e.parent);
            if (!e.parent.empty() && parent < 0) return fail(std::format("reparent '{}': parent '{}' not found", e.bone, e.parent));
            if (auto st = reparent_node(m, node, parent, o, r); !st) return st.error();
            r.reparented.push_back(e.bone);
            break;
        }
        case BoneEdit::Op::Remove: {
            if (node < 0) return fail(std::format("remove: bone '{}' not found", e.bone));
            if (is_protected(o, e.bone) && !e.force) return fail(std::format("remove: '{}' is protected; set force", e.bone));
            const ModelNode& victim = m.nodes[static_cast<std::size_t>(node)];
            if (!victim.meshes.empty() || victim.light >= 0 || victim.camera >= 0)
                return fail(std::format("remove: '{}' carries meshes, a light or a camera", e.bone));
            // Children move to the removed bone's parent, keeping their world transforms.
            const std::int32_t up = victim.parent;
            for (std::size_t i = 0; i < m.nodes.size(); ++i) {
                const std::int32_t cur = find_node(m, e.bone);
                if (m.nodes[i].parent == cur)
                    if (auto st = reparent_node(m, static_cast<std::int32_t>(i), up, o, r); !st) return st.error();
            }
            const std::int32_t gone = find_node(m, e.bone);
            // Weights go to the named joint or the nearest ancestor joint, in every skin.
            for (std::size_t si = 0; si < m.skins.size(); ++si) {
                ModelSkin& s = m.skins[si];
                const std::int32_t j = joint_of(s, gone);
                if (j < 0) continue;
                std::int32_t to = -1;
                if (!e.name.empty()) {
                    const std::int32_t tn = find_node(m, e.name);
                    to = tn >= 0 ? joint_of(s, tn) : -1;
                    if (to < 0) return fail(std::format("remove '{}': weight target '{}' is not a joint", e.bone, e.name));
                } else {
                    for (std::int32_t p = m.nodes[static_cast<std::size_t>(gone)].parent; p >= 0 && to < 0;
                         p = m.nodes[static_cast<std::size_t>(p)].parent)
                        to = joint_of(s, p);
                    if (to < 0) return fail(std::format("remove '{}': no ancestor joint receives its weights; name one", e.bone));
                }
                for (const std::uint32_t mi : skin_meshes(m, static_cast<std::uint32_t>(si))) {
                    for (render::SkinVertex& sv : m.meshes[mi].data.skin) {
                        float w = 0;
                        for (int k = 0; k < 4; ++k)
                            if (sv.joints[k] == static_cast<std::uint16_t>(j)) w += sv.weights[k];
                        if (!(w > 0.0f)) continue;
                        sv = add_influence(sv, static_cast<std::uint32_t>(to), w, static_cast<std::uint32_t>(j), w);
                        ++r.vertices_reweighted;
                    }
                }
                if (o.compact_joints) {
                    s.joints.erase(s.joints.begin() + j);
                    s.inverse_bind.erase(s.inverse_bind.begin() + j);
                    for (const std::uint32_t mi : skin_meshes(m, static_cast<std::uint32_t>(si)))
                        for (render::SkinVertex& sv : m.meshes[mi].data.skin)
                            for (std::uint16_t& jj : sv.joints)
                                if (jj > j) --jj;
                    if (si == o.skin)
                        for (std::int32_t& v : r.joint_remap) {
                            if (v == j) v = -1;
                            else if (v > j) --v;
                        }
                } else {
                    // Keep the slot: it now follows the receiving joint exactly.
                    s.joints[static_cast<std::size_t>(j)] = s.joints[static_cast<std::size_t>(to)];
                    s.inverse_bind[static_cast<std::size_t>(j)] = s.inverse_bind[static_cast<std::size_t>(to)];
                }
            }
            // Drop the node and its channels, renumbering the nodes after it.
            for (animation::Clip& c : m.animations) {
                const std::size_t before = c.channels.size();
                std::erase_if(c.channels, [&](const animation::Channel& ch) { return ch.node == static_cast<std::uint32_t>(gone); });
                r.channels_dropped += before - c.channels.size();
            }
            std::vector<std::int32_t> step(m.nodes.size());
            for (std::size_t i = 0; i < m.nodes.size(); ++i)
                step[i] = static_cast<std::int32_t>(i) == gone ? -1 : static_cast<std::int32_t>(i) - (static_cast<std::int32_t>(i) > gone ? 1 : 0);
            m.nodes.erase(m.nodes.begin() + gone);
            for (ModelNode& nd : m.nodes)
                if (nd.parent >= 0) nd.parent = step[static_cast<std::size_t>(nd.parent)];
            for (ModelSkin& s : m.skins)
                for (std::uint32_t& jn : s.joints) jn = static_cast<std::uint32_t>(step[jn]);
            for (animation::Clip& c : m.animations)
                for (animation::Channel& ch : c.channels) ch.node = static_cast<std::uint32_t>(step[ch.node]);
            compose_map(r.node_remap, step);
            r.removed.push_back(e.bone);
            break;
        }
        }
    }
    if (auto st = validate_model(m); !st) return make_error(ErrorCode::InvalidArgument, "skeleton edit produced an invalid model: " + st.error().message);
    r.layout_after = skeleton_layout_hash(m, o.skin);
    model = std::move(m);
    return r;
}

// ---- migrations --------------------------------------------------------------------------------

namespace {

const char* op_name(BoneEdit::Op op) {
    switch (op) {
    case BoneEdit::Op::Add: return "add";
    case BoneEdit::Op::Rename: return "rename";
    case BoneEdit::Op::Reparent: return "reparent";
    case BoneEdit::Op::Remove: return "remove";
    }
    return "add";
}

json::Value vec(std::initializer_list<float> v) {
    json::Value a = json::Value::array();
    for (const float f : v) a.push(f);
    return a;
}

bool read_floats(const json::Value* v, float* out, std::size_t n) {
    if (!v) return true;
    if (!v->is_array() || v->as_array().size() != n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        if (!(*v)[i].is_number()) return false;
        out[i] = (*v)[i].as_float();
        if (!std::isfinite(out[i])) return false;
    }
    return true;
}

} // namespace

json::Value SkeletonMigration::to_json() const {
    json::Value v = json::Value::object();
    v.set("format", "victus.skeleton_migration");
    v.set("profile", profile);
    v.set("from_version", from_version);
    v.set("to_version", to_version);
    v.set("requires_layout", requires_layout ? std::format("{:016x}", requires_layout) : std::string());
    if (!description.empty()) v.set("description", description);
    json::Value list = json::Value::array();
    for (const BoneEdit& e : edits) {
        json::Value o = json::Value::object();
        o.set("op", op_name(e.op));
        o.set("bone", e.bone);
        if (!e.parent.empty() || e.op == BoneEdit::Op::Add) o.set("parent", e.parent);
        if (!e.name.empty()) o.set("name", e.name);
        if (e.op == BoneEdit::Op::Add) {
            o.set("translation", vec({e.translation.x, e.translation.y, e.translation.z}));
            o.set("rotation", vec({e.rotation.x, e.rotation.y, e.rotation.z, e.rotation.w}));
            o.set("scale", vec({e.scale.x, e.scale.y, e.scale.z}));
            o.set("deform", e.deform);
            if (!e.weights_from.empty()) {
                o.set("weights_from", e.weights_from);
                o.set("weight_share", e.weight_share);
                o.set("weight_start", e.weight_start);
                o.set("weight_end", e.weight_end);
            }
        }
        if (e.force) o.set("force", true);
        list.push(std::move(o));
    }
    v.set("edits", std::move(list));
    return v;
}

Result<SkeletonMigration> SkeletonMigration::from_json(const json::Value& v) {
    const auto bad = [](std::string m) { return make_error(ErrorCode::ParseError, "skeleton migration: " + std::move(m)); };
    if (!v.is_object()) return bad("not an object");
    if (const auto* f = v.find("format"); f && f->as_string_or("") != "victus.skeleton_migration") return bad("unknown format");
    SkeletonMigration m;
    m.profile = v["profile"].as_string_or("");
    m.from_version = static_cast<std::uint32_t>(std::clamp<std::int64_t>(v["from_version"].as_int(1), 0, 1 << 30));
    m.to_version = static_cast<std::uint32_t>(std::clamp<std::int64_t>(v["to_version"].as_int(2), 0, 1 << 30));
    if (m.to_version <= m.from_version) return bad("to_version must be greater than from_version");
    m.description = v["description"].as_string_or("");
    const std::string layout = v["requires_layout"].as_string_or("");
    if (!layout.empty()) {
        if (layout.size() > 16) return bad("requires_layout is not a 64-bit hex value");
        std::uint64_t h = 0;
        for (const char c : layout) {
            const int d = std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
                          : (c >= 'a' && c <= 'f')                     ? c - 'a' + 10
                          : (c >= 'A' && c <= 'F')                     ? c - 'A' + 10
                                                                       : -1;
            if (d < 0) return bad("requires_layout is not hex");
            h = (h << 4) | static_cast<std::uint64_t>(d);
        }
        m.requires_layout = h;
    }
    const json::Value* edits = v.find("edits");
    if (!edits || !edits->is_array()) return bad("missing edits");
    if (edits->as_array().size() > 4096) return bad("too many edits");
    for (const json::Value& o : edits->as_array()) {
        if (!o.is_object()) return bad("edit is not an object");
        BoneEdit e;
        const std::string op = o["op"].as_string_or("");
        if (op == "add") e.op = BoneEdit::Op::Add;
        else if (op == "rename") e.op = BoneEdit::Op::Rename;
        else if (op == "reparent") e.op = BoneEdit::Op::Reparent;
        else if (op == "remove") e.op = BoneEdit::Op::Remove;
        else return bad("unknown op '" + op + "'");
        e.bone = o["bone"].as_string_or("");
        if (e.bone.empty() || e.bone.size() > 256) return bad("edit without a valid bone name");
        e.parent = o["parent"].as_string_or("");
        e.name = o["name"].as_string_or("");
        float t[3]{0, 0, 0}, q[4]{0, 0, 0, 1}, s[3]{1, 1, 1};
        if (!read_floats(o.find("translation"), t, 3) || !read_floats(o.find("rotation"), q, 4) || !read_floats(o.find("scale"), s, 3))
            return bad("bad transform on '" + e.bone + "'");
        e.translation = {t[0], t[1], t[2]};
        e.rotation = {q[0], q[1], q[2], q[3]};
        e.scale = {s[0], s[1], s[2]};
        e.deform = o["deform"].as_bool(true);
        e.weights_from = o["weights_from"].as_string_or("");
        e.weight_share = math::saturate(o["weight_share"].as_float(0.0f));
        e.weight_start = o["weight_start"].as_float(0.0f);
        e.weight_end = o["weight_end"].as_float(1.0f);
        e.force = o["force"].as_bool(false);
        m.edits.push_back(std::move(e));
    }
    return m;
}

Result<SkeletonMigration> parse_skeleton_migration(std::string_view text) {
    auto v = json::parse(text);
    if (!v) return v.error();
    return SkeletonMigration::from_json(*v);
}

std::string write_skeleton_migration(const SkeletonMigration& migration) { return json::stringify(migration.to_json()); }

Result<SkeletonEditReport> apply_skeleton_migration(CookedModel& model, const SkeletonMigration& migration,
                                                    const SkeletonEditOptions& options) {
    const std::uint64_t have = skeleton_layout_hash(model, options.skin);
    if (migration.requires_layout != 0 && migration.requires_layout != have)
        return make_error(ErrorCode::VersionMismatch,
                          std::format("skeleton migration {} v{}->v{} expects layout {:016x}, the model has {:016x}", migration.profile,
                                      migration.from_version, migration.to_version, migration.requires_layout, have));
    return apply_skeleton_edits(model, migration.edits, options);
}

Result<void> apply_import_skeleton_migrations(CookedModel& model, const ModelImportSettings& settings, const fs::Path& model_path,
                                              const fs::Path& sandbox_root, std::vector<fs::Path>& dependencies) {
    std::error_code ec;
    const fs::Path root = std::filesystem::weakly_canonical(sandbox_root.empty() ? model_path.parent_path() : sandbox_root, ec);
    for (const std::string& rel : settings.skeleton_migrations) {
        if (rel.find("://") != std::string::npos)
            return make_error(ErrorCode::InvalidArgument, std::format("skeleton migration '{}': remote paths are not allowed", rel));
        const fs::Path p = std::filesystem::weakly_canonical(model_path.parent_path() / fs::Path(std::u8string(rel.begin(), rel.end())), ec);
        const auto [ri, pi] = std::mismatch(root.begin(), root.end(), p.begin(), p.end());
        if (ec || ri != root.end())
            return make_error(ErrorCode::InvalidArgument, std::format("skeleton migration '{}' escapes the asset directory", rel));
        dependencies.push_back(p);
        auto text = fs::read_text(p, 16u << 20);
        if (!text) return make_error(text.error().code, std::format("skeleton migration '{}': {}", rel, text.error().message));
        auto mig = parse_skeleton_migration(*text);
        if (!mig) return make_error(mig.error().code, std::format("skeleton migration '{}': {}", rel, mig.error().message));
        auto applied = apply_skeleton_migration(model, *mig);
        if (!applied) return make_error(applied.error().code, std::format("skeleton migration '{}': {}", rel, applied.error().message));
    }
    return {};
}

// ---- compatibility -----------------------------------------------------------------------------

bool SkeletonCompatibility::ok() const noexcept {
    return std::none_of(issues.begin(), issues.end(),
                        [](const SkeletonCompatibilityIssue& i) { return i.severity == SkeletonCompatibilityIssue::Severity::Error; });
}

SkeletonCompatibility check_skeleton_compatibility(const CookedModel& before, const CookedModel& after, const CompatibilityOptions& o) {
    using Sev = SkeletonCompatibilityIssue::Severity;
    SkeletonCompatibility out;
    const auto add = [&](Sev s, std::string bone, std::string msg) { out.issues.push_back({s, std::move(bone), std::move(msg)}); };
    std::map<std::string, std::string> rename(o.renames.begin(), o.renames.end());
    const auto mapped = [&](const std::string& n) {
        const auto it = rename.find(n);
        return it == rename.end() ? n : it->second;
    };
    const SkeletonInfo a = describe_skeleton(before, o.skin), b = describe_skeleton(after, o.skin);
    if (b.bones.empty() && !a.bones.empty()) {
        add(Sev::Error, "", "the new model has no skeleton for this skin");
        return out;
    }
    std::set<std::string> seen;
    for (const SkeletonBone& old : a.bones) {
        const std::string name = mapped(old.name);
        const SkeletonBone* now = b.find(name);
        if (!now) {
            ++out.missing_bones;
            add(Sev::Error, old.name, "bone is missing (scripts, sockets, retarget maps and clips refer to it by name)");
            continue;
        }
        seen.insert(name);
        ++out.preserved_bones;
        const std::string old_parent = old.parent_node >= 0 ? mapped(before.nodes[static_cast<std::size_t>(old.parent_node)].name) : "";
        const std::string new_parent = now->parent_node >= 0 ? after.nodes[static_cast<std::size_t>(now->parent_node)].name : "";
        if (old_parent != new_parent)
            add(Sev::Warning, old.name, std::format("parent changed from '{}' to '{}'", old_parent, new_parent));
        const float moved = math::length(now->rest_position - old.rest_position);
        const bool socket = (old.roles & kBoneSocket) != 0;
        if (moved > (socket ? o.socket_tolerance : o.rest_tolerance))
            add(socket ? Sev::Error : Sev::Warning, old.name, std::format("bind position moved {:.4f} m", moved));
        if (old.joint >= 0 && now->joint != old.joint)
            add(Sev::Error, old.name,
                now->joint < 0 ? std::string("no longer a skin joint")
                               : std::format("skin joint index changed {} -> {} (baked joint indices must be rebuilt)", old.joint, now->joint));
    }
    for (const SkeletonBone& nb : b.bones)
        if (!seen.count(nb.name)) {
            ++out.added_bones;
            add(Sev::Info, nb.name, std::format("added ({})", bone_roles_string(nb.roles).empty() ? "bone" : bone_roles_string(nb.roles)));
        }
    // Clips keep their channels on preserved bones.
    for (const auto& clip : before.animations) {
        const auto it = std::find_if(after.animations.begin(), after.animations.end(), [&](const auto& c) { return c.name == clip.name; });
        if (it == after.animations.end()) {
            add(Sev::Error, "", std::format("clip '{}' is missing", clip.name));
            continue;
        }
        std::set<std::pair<std::string, int>> have;
        for (const auto& ch : it->channels)
            if (ch.node < after.nodes.size()) have.insert({after.nodes[ch.node].name, static_cast<int>(ch.path)});
        for (const auto& ch : clip.channels) {
            if (ch.node >= before.nodes.size()) continue;
            const std::string n = mapped(before.nodes[ch.node].name);
            if (b.find(n) && !have.count({n, static_cast<int>(ch.path)}))
                add(Sev::Warning, n, std::format("clip '{}' lost a channel on this bone", clip.name));
        }
    }
    return out;
}

// ---- automatic weights -------------------------------------------------------------------------

std::size_t auto_skin_weights(CookedModel& model, const AutoWeightSettings& s) {
    if (s.skin >= model.skins.size()) return 0;
    const ModelSkin& skin = model.skins[s.skin];
    const std::vector<Mat4> bind = bind_transforms(model, s.skin);
    struct Seg {
        Vec3 a, b;
        std::uint32_t joint;
    };
    std::vector<Seg> segs;
    for (std::size_t j = 0; j < skin.joints.size(); ++j) {
        const std::uint32_t nd = skin.joints[j];
        if (nd >= model.nodes.size()) continue;
        const std::string& name = model.nodes[nd].name;
        if (!s.only.empty() && std::find(s.only.begin(), s.only.end(), name) == s.only.end()) continue;
        const std::uint32_t roles = infer_bone_roles(name);
        if (roles & (kBoneIkTarget | kBoneIkPole | kBoneSocket)) continue;
        const Vec3 head = position(bind[nd]);
        Vec3 tail = head;
        bool found = false;
        for (std::size_t i = 0; i < model.nodes.size() && !found; ++i)
            if (model.nodes[i].parent == static_cast<std::int32_t>(nd) && joint_of(skin, static_cast<std::int32_t>(i)) >= 0) {
                tail = position(bind[i]);
                found = true;
            }
        if (!found) {
            // Leaf: a short segment along the bone's +Y (glTF/Blender bone axis) scaled by its parent's length.
            float plen = 0.05f;
            if (const std::int32_t p = model.nodes[nd].parent; p >= 0) plen = std::max(0.02f, 0.5f * math::length(head - position(bind[static_cast<std::size_t>(p)])));
            tail = head + math::normalize(bind[nd].transform_vector({0, 1, 0})) * plen;
        }
        segs.push_back({head, tail, static_cast<std::uint32_t>(j)});
    }
    if (segs.empty()) return 0;
    std::size_t changed = 0;
    for (const std::uint32_t mi : skin_meshes(model, s.skin, false)) {
        render::MeshData& d = model.meshes[mi].data;
        for (std::size_t vi = 0; vi < d.vertices.size(); ++vi) {
            const Vec3 p = d.vertices[vi].position;
            std::vector<std::pair<float, std::uint32_t>> best;
            best.reserve(segs.size());
            for (const Seg& sg : segs) {
                const Vec3 ab = sg.b - sg.a;
                const float l2 = math::dot(ab, ab);
                const float t = l2 > 1e-12f ? math::saturate(math::dot(p - sg.a, ab) / l2) : 0.0f;
                best.push_back({math::length(p - (sg.a + ab * t)), sg.joint});
            }
            const std::size_t k = std::min<std::size_t>(4, best.size());
            std::partial_sort(best.begin(), best.begin() + static_cast<std::ptrdiff_t>(k), best.end());
            std::uint32_t j[4];
            float w[4];
            for (std::size_t i = 0; i < k; ++i) {
                j[i] = best[i].second;
                w[i] = 1.0f / std::pow(best[i].first + s.epsilon, s.falloff);
            }
            d.skin.resize(d.vertices.size());
            d.skin[vi] = render::make_skin_vertex(j, w, k);
            ++changed;
        }
    }
    return changed;
}

} // namespace victus::assets
