// SPDX-License-Identifier: Apache-2.0
#include "victus/animation/retarget.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace victus::animation {
using namespace math;

namespace {

struct World {
    std::vector<Quat> rotation;
    std::vector<Vec3> position;
};

// World transforms of a pose (nodes are parent-before-child). Scale is ignored for rotations and
// applied to positions.
void world_of(const AnimationSet& set, std::span<const NodePose> pose, World& w) {
    const std::size_t n = set.nodes.size();
    w.rotation.assign(n, Quat{});
    w.position.assign(n, Vec3{});
    std::vector<Vec3> scale(n, Vec3{1, 1, 1});
    for (std::size_t i = 0; i < n; ++i) {
        const NodePose& p = pose[i];
        const std::int32_t parent = set.nodes[i].parent;
        if (parent < 0 || static_cast<std::size_t>(parent) >= i) {
            w.rotation[i] = normalize(p.rotation);
            w.position[i] = p.translation;
            scale[i] = p.scale;
            continue;
        }
        const auto pi = static_cast<std::size_t>(parent);
        const Vec3 s = scale[pi];
        w.rotation[i] = normalize(w.rotation[pi] * p.rotation);
        w.position[i] = w.position[pi] + w.rotation[pi].rotate({p.translation.x * s.x, p.translation.y * s.y, p.translation.z * s.z});
        scale[i] = {s.x * p.scale.x, s.y * p.scale.y, s.z * p.scale.z};
    }
}

std::vector<NodePose> rest_pose(const AnimationSet& set) {
    std::vector<NodePose> out(set.nodes.size());
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = set.nodes[i].rest;
    return out;
}

Quat parent_rot(const AnimationSet& set, const World& w, std::size_t i) {
    const std::int32_t p = set.nodes[i].parent;
    return p >= 0 && static_cast<std::size_t>(p) < i ? w.rotation[static_cast<std::size_t>(p)] : Quat{};
}

Quat sign_align(Quat q, Quat ref) {
    return ref.x * q.x + ref.y * q.y + ref.z * q.z + ref.w * q.w < 0 ? Quat{-q.x, -q.y, -q.z, -q.w} : q;
}

} // namespace

std::string normalize_bone_name(std::string_view name) {
    if (const auto cut = name.find_last_of(":|"); cut != std::string_view::npos) name.remove_prefix(cut + 1);
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '_' || c == ' ' || c == '.' || c == '-') continue;
        out.push_back(static_cast<char>(std::tolower(u)));
    }
    return out;
}

std::vector<std::int32_t> map_bones(const AnimationSet& source, const AnimationSet& target, const RetargetOptions& options) {
    std::vector<std::int32_t> map(target.nodes.size(), -1);
    auto find_exact = [&](std::string_view n) -> std::int32_t {
        for (std::size_t i = 0; i < source.nodes.size(); ++i)
            if (source.nodes[i].name == n) return static_cast<std::int32_t>(i);
        return -1;
    };
    std::vector<std::string> norm(source.nodes.size());
    for (std::size_t i = 0; i < source.nodes.size(); ++i) norm[i] = normalize_bone_name(source.nodes[i].name);
    // Explicit mappings claim their source bones first; automatic matches fill in the rest, and a
    // source bone drives at most one target bone (the first in hierarchy order).
    std::vector<std::uint8_t> used(source.nodes.size(), 0), fixed(target.nodes.size(), 0);
    for (std::size_t t = 0; t < target.nodes.size(); ++t)
        for (const auto& [to, from] : options.bone_map)
            if (to == target.nodes[t].name) {
                const std::int32_t s = find_exact(from);
                fixed[t] = 1;
                if (s >= 0 && !used[static_cast<std::size_t>(s)]) {
                    map[t] = s;
                    used[static_cast<std::size_t>(s)] = 1;
                }
                break;
            }
    const auto claim = [&](std::size_t t, std::int32_t s) {
        if (s < 0 || used[static_cast<std::size_t>(s)]) return false;
        map[t] = s;
        used[static_cast<std::size_t>(s)] = 1;
        return true;
    };
    for (std::size_t t = 0; t < target.nodes.size(); ++t) {
        if (fixed[t]) continue;
        const std::string& tn = target.nodes[t].name;
        if (claim(t, find_exact(tn))) continue;
        const std::string n = normalize_bone_name(tn);
        if (n.empty()) continue;
        for (std::size_t i = 0; i < norm.size(); ++i)
            if (norm[i] == n && claim(t, static_cast<std::int32_t>(i))) break;
    }
    return map;
}

Clip retarget_clip(const AnimationSet& source, const Clip& clip, const AnimationSet& target,
                   std::span<const std::int32_t> map, const RetargetOptions& options) {
    Clip out;
    out.name = clip.name;
    out.duration = clip.duration;
    const std::size_t ns = source.nodes.size(), nt = target.nodes.size();
    if (map.size() != nt || ns == 0 || nt == 0) return out;

    // Key times: the union of the clip's key times, or a uniform grid when there are too many.
    std::vector<float> times;
    for (const Channel& ch : clip.channels) times.insert(times.end(), ch.times.begin(), ch.times.end());
    times.push_back(0.0f);
    times.push_back(clip.duration);
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end(), [](float a, float b) { return std::abs(a - b) < 1e-5f; }), times.end());
    const float rate = std::max(options.max_sample_rate, 1.0f);
    const auto cap = static_cast<std::size_t>(std::ceil(clip.duration * rate)) + 2;
    if (times.size() > cap) {
        times.clear();
        const std::size_t steps = cap - 1;
        for (std::size_t k = 0; k <= steps; ++k)
            times.push_back(clip.duration * static_cast<float>(k) / static_cast<float>(steps));
    }

    // Which source bones the clip animates; the root bone is the first mapped target bone (in
    // hierarchy order) whose source bone has a translation channel.
    std::vector<std::uint8_t> src_rot(ns, 0), src_tr(ns, 0);
    for (const Channel& ch : clip.channels) {
        if (ch.node >= ns) continue;
        if (ch.path == Path::Rotation) src_rot[ch.node] = 1;
        if (ch.path == Path::Translation) src_tr[ch.node] = 1;
    }
    std::int32_t root = -1;
    if (options.root_translation)
        for (std::size_t t = 0; t < nt && root < 0; ++t)
            if (map[t] >= 0 && src_tr[static_cast<std::size_t>(map[t])]) root = static_cast<std::int32_t>(t);

    const std::vector<NodePose> src_rest = rest_pose(source), tgt_rest = rest_pose(target);
    World ws0, wt0;
    world_of(source, src_rest, ws0);
    world_of(target, tgt_rest, wt0);
    float scale = 1.0f;
    if (root >= 0) {
        const auto r = static_cast<std::size_t>(root);
        const Vec3 ps = ws0.position[static_cast<std::size_t>(map[r])], pt = wt0.position[r];
        const float ls = std::sqrt(dot(ps, ps)), lt = std::sqrt(dot(pt, pt));
        if (ls > 1e-5f && lt > 1e-5f) scale = lt / ls;
    }

    // Target bones that receive channels: mapped bones with an animated source bone or an animated
    // source ancestor (their world rotation changes), excluding unmapped bones (they keep rest).
    std::vector<std::uint8_t> moves(ns, 0);
    for (std::size_t i = 0; i < ns; ++i) {
        const std::int32_t p = source.nodes[i].parent;
        moves[i] = src_rot[i] || (p >= 0 && static_cast<std::size_t>(p) < i && moves[static_cast<std::size_t>(p)]);
    }
    std::vector<std::int32_t> channel_of(nt, -1);
    for (std::size_t t = 0; t < nt; ++t) {
        if (map[t] < 0) continue;
        const auto s = static_cast<std::size_t>(map[t]);
        bool target_parent_moves = false;
        for (std::int32_t p = target.nodes[t].parent; p >= 0 && static_cast<std::size_t>(p) < t;
             p = target.nodes[static_cast<std::size_t>(p)].parent)
            if (channel_of[static_cast<std::size_t>(p)] >= 0) {
                target_parent_moves = true;
                break;
            }
        if (!moves[s] && !target_parent_moves) continue;
        channel_of[t] = static_cast<std::int32_t>(out.channels.size());
        Channel ch{.node = static_cast<std::uint32_t>(t), .path = Path::Rotation, .interpolation = Interpolation::Linear, .times = times};
        ch.values.reserve(times.size() * 4);
        out.channels.push_back(std::move(ch));
    }
    std::int32_t root_channel = -1;
    if (root >= 0) {
        root_channel = static_cast<std::int32_t>(out.channels.size());
        Channel ch{.node = static_cast<std::uint32_t>(root), .path = Path::Translation, .interpolation = Interpolation::Linear, .times = times};
        ch.values.reserve(times.size() * 3);
        out.channels.push_back(std::move(ch));
    }

    std::vector<NodePose> src_pose(ns);
    World ws;
    std::vector<Quat> wt(nt);
    std::vector<Quat> last(nt);
    for (std::size_t k = 0; k < times.size(); ++k) {
        src_pose = src_rest;
        sample(clip, times[k], src_pose);
        world_of(source, src_pose, ws);
        for (std::size_t t = 0; t < nt; ++t) {
            const std::int32_t tp = target.nodes[t].parent;
            const Quat parent = tp >= 0 && static_cast<std::size_t>(tp) < t ? wt[static_cast<std::size_t>(tp)] : Quat{};
            Quat local = tgt_rest[t].rotation;
            if (map[t] >= 0) {
                const auto s = static_cast<std::size_t>(map[t]);
                // World delta from rest, applied to the target's world rest rotation.
                const Quat delta = normalize(ws.rotation[s] * ws0.rotation[s].conjugate());
                const Quat world = normalize(delta * wt0.rotation[t]);
                local = normalize(parent.conjugate() * world);
            }
            wt[t] = normalize(parent * local);
            if (const std::int32_t c = channel_of[t]; c >= 0) {
                if (k > 0) local = sign_align(local, last[t]); // keep interpolation on the short path
                last[t] = local;
                auto& v = out.channels[static_cast<std::size_t>(c)].values;
                v.insert(v.end(), {local.x, local.y, local.z, local.w});
            }
        }
        if (root_channel >= 0) {
            const auto r = static_cast<std::size_t>(root);
            const auto s = static_cast<std::size_t>(map[r]);
            const Vec3 d = src_pose[s].translation - src_rest[s].translation;
            const Vec3 world_d = parent_rot(source, ws0, s).rotate(d) * scale;
            const Vec3 local_d = parent_rot(target, wt0, r).conjugate().rotate(world_d);
            const Vec3 p = tgt_rest[r].translation + local_d;
            auto& v = out.channels[static_cast<std::size_t>(root_channel)].values;
            v.insert(v.end(), {p.x, p.y, p.z});
        }
    }
    return out;
}

std::shared_ptr<AnimationSet> retarget_set(const AnimationSet& source, const AnimationSet& target,
                                           const RetargetOptions& options, RetargetReport* report) {
    const std::vector<std::int32_t> map = map_bones(source, target, options);
    RetargetReport rep;
    rep.target_bones = static_cast<std::uint32_t>(target.nodes.size());
    for (std::size_t t = 0; t < map.size(); ++t) {
        if (map[t] >= 0) ++rep.mapped_bones;
        else if (rep.unmapped.size() < 32) rep.unmapped.push_back(target.nodes[t].name);
    }
    if (rep.mapped_bones == 0) {
        rep.skipped_clips = static_cast<std::uint32_t>(source.clips.size());
        if (report) *report = std::move(rep);
        return nullptr;
    }
    auto out = std::make_shared<AnimationSet>();
    out->nodes = target.nodes;
    out->clips = target.clips;
    for (const Clip& c : source.clips) {
        if (out->find_clip(c.name) >= 0) {
            ++rep.skipped_clips; // the target's own clip of that name wins
            continue;
        }
        out->clips.push_back(retarget_clip(source, c, target, map, options));
        ++rep.clips;
    }
    if (report) *report = std::move(rep);
    return out;
}

} // namespace victus::animation
