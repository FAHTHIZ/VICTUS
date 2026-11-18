// SPDX-License-Identifier: Apache-2.0
#include "victus/animation/animation.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace victus::animation {

using namespace math;

std::int32_t AnimationSet::find_clip(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < clips.size(); ++i)
        if (clips[i].name == name) return static_cast<std::int32_t>(i);
    return -1;
}

Status validate(const Clip& clip, std::size_t node_count) {
    for (std::size_t ci = 0; ci < clip.channels.size(); ++ci) {
        const Channel& ch = clip.channels[ci];
        const auto where = [&] { return std::format("clip '{}' channel {}", clip.name, ci); };
        if (ch.node >= node_count) return make_error(ErrorCode::InvalidArgument, where() + ": node index out of range");
        if (ch.times.empty()) return make_error(ErrorCode::InvalidArgument, where() + ": no keys");
        if (ch.path > Path::Weights) return make_error(ErrorCode::InvalidArgument, where() + ": unknown path");
        if (ch.path == Path::Weights && (ch.width == 0 || ch.width > 256))
            return make_error(ErrorCode::InvalidArgument, where() + ": morph weight channel needs 1..256 targets");
        const std::size_t per_key = ch.components() * (ch.interpolation == Interpolation::CubicSpline ? 3u : 1u);
        if (ch.values.size() != ch.times.size() * per_key)
            return make_error(ErrorCode::InvalidArgument, where() + ": value count does not match key count");
        for (std::size_t i = 0; i < ch.times.size(); ++i) {
            if (!std::isfinite(ch.times[i]) || ch.times[i] < 0.0f || (i > 0 && ch.times[i] < ch.times[i - 1]))
                return make_error(ErrorCode::InvalidArgument, where() + ": key times must be finite, >= 0 and sorted");
        }
        for (const float v : ch.values)
            if (!std::isfinite(v)) return make_error(ErrorCode::InvalidArgument, where() + ": non-finite value");
    }
    if (!std::isfinite(clip.duration) || clip.duration < 0.0f)
        return make_error(ErrorCode::InvalidArgument, std::format("clip '{}': invalid duration", clip.name));
    return {};
}

void expand_morph_weights(AnimationSet& set, std::span<const MorphNode> morphs) {
    // First weight node of each morphing node, and its target count.
    std::vector<std::int32_t> first(set.nodes.size(), -1);
    std::vector<std::uint32_t> count(set.nodes.size(), 0);
    for (const MorphNode& m : morphs) {
        if (m.node >= first.size() || first[m.node] >= 0 || m.names.empty()) continue;
        first[m.node] = static_cast<std::int32_t>(set.nodes.size());
        count[m.node] = static_cast<std::uint32_t>(m.names.size());
        const std::string base = set.nodes[m.node].name;
        for (std::size_t t = 0; t < m.names.size(); ++t) {
            AnimationSet::Node n;
            n.name = base + "/" + m.names[t];
            n.parent = static_cast<std::int32_t>(m.node);
            n.rest.translation = {t < m.defaults.size() ? m.defaults[t] : 0.0f, 0, 0};
            n.morph_node = static_cast<std::int32_t>(m.node);
            n.morph_index = static_cast<std::uint32_t>(t);
            set.nodes.push_back(std::move(n));
        }
    }
    for (Clip& clip : set.clips) {
        std::vector<Channel> out;
        out.reserve(clip.channels.size());
        for (Channel& ch : clip.channels) {
            if (ch.path != Path::Weights) {
                out.push_back(std::move(ch));
                continue;
            }
            if (ch.node >= first.size() || first[ch.node] < 0 || ch.width == 0 || ch.width > count[ch.node]) continue;
            const std::size_t per = ch.interpolation == Interpolation::CubicSpline ? 3 : 1;
            const std::size_t keys = ch.times.size();
            if (ch.values.size() != keys * per * ch.width) continue;
            for (std::uint32_t t = 0; t < ch.width; ++t) {
                Channel w{.node = static_cast<std::uint32_t>(first[ch.node]) + t, .path = Path::Translation,
                          .interpolation = ch.interpolation, .times = ch.times, .values = {}, .width = 0};
                w.values.reserve(keys * per * 3);
                for (std::size_t k = 0; k < keys * per; ++k) w.values.insert(w.values.end(), {ch.values[k * ch.width + t], 0.0f, 0.0f});
                out.push_back(std::move(w));
            }
        }
        clip.channels = std::move(out);
    }
}

float wrap_time(float time, float duration, bool loop) noexcept {
    if (!(duration > 0.0f)) return 0.0f;
    if (!loop) return std::clamp(time, 0.0f, duration);
    float t = std::fmod(time, duration);
    if (t < 0.0f) t += duration;
    return t;
}

namespace {

// Key interval containing `time`: i such that times[i] <= time < times[i + 1], and the
// normalized position u within it. Clamps outside the key range.
struct Span {
    std::size_t i{0};
    float u{0}, dt{0};
    bool single{true};
};

Span locate(const std::vector<float>& times, float time) noexcept {
    Span s;
    const std::size_t n = times.size();
    if (n <= 1 || time <= times.front()) return s;
    if (time >= times.back()) {
        s.i = n - 1;
        return s;
    }
    const auto it = std::upper_bound(times.begin(), times.end(), time);
    s.i = static_cast<std::size_t>(it - times.begin()) - 1;
    s.dt = times[s.i + 1] - times[s.i];
    s.u = s.dt > 0.0f ? (time - times[s.i]) / s.dt : 0.0f;
    s.single = false;
    return s;
}

// Value of component c of key k (cubic splines skip the in-tangent).
float key_value(const Channel& ch, std::size_t k, std::uint32_t c) noexcept {
    const std::uint32_t n = ch.components();
    if (ch.interpolation == Interpolation::CubicSpline) return ch.values[(k * 3 + 1) * n + c];
    return ch.values[k * n + c];
}

// Hermite spline (glTF 2.0 Appendix C): p(u) = (2u^3-3u^2+1)v0 + (u^3-2u^2+u)dt*b0 +
// (-2u^3+3u^2)v1 + (u^3-u^2)dt*a1, with b0 the out-tangent of key 0 and a1 the in-tangent of key 1.
float cubic(const Channel& ch, const Span& s, std::uint32_t c) noexcept {
    const std::uint32_t n = ch.components();
    const float u = s.u, u2 = u * u, u3 = u2 * u;
    const float v0 = ch.values[(s.i * 3 + 1) * n + c];
    const float b0 = ch.values[(s.i * 3 + 2) * n + c];
    const float a1 = ch.values[((s.i + 1) * 3 + 0) * n + c];
    const float v1 = ch.values[((s.i + 1) * 3 + 1) * n + c];
    return (2 * u3 - 3 * u2 + 1) * v0 + (u3 - 2 * u2 + u) * s.dt * b0 + (-2 * u3 + 3 * u2) * v1 + (u3 - u2) * s.dt * a1;
}

} // namespace

Vec3 sample_vec3(const Channel& ch, float time) noexcept {
    if (ch.times.empty()) return {};
    const Span s = locate(ch.times, time);
    float out[3];
    for (std::uint32_t c = 0; c < 3; ++c) {
        if (s.single || ch.interpolation == Interpolation::Step) out[c] = key_value(ch, s.i, c);
        else if (ch.interpolation == Interpolation::Linear)
            out[c] = key_value(ch, s.i, c) + (key_value(ch, s.i + 1, c) - key_value(ch, s.i, c)) * s.u;
        else out[c] = cubic(ch, s, c);
    }
    return {out[0], out[1], out[2]};
}

Quat sample_quat(const Channel& ch, float time) noexcept {
    if (ch.times.empty()) return {};
    const Span s = locate(ch.times, time);
    auto key = [&](std::size_t k) {
        return Quat{key_value(ch, k, 0), key_value(ch, k, 1), key_value(ch, k, 2), key_value(ch, k, 3)};
    };
    if (s.single || ch.interpolation == Interpolation::Step) return normalize(key(s.i));
    if (ch.interpolation == Interpolation::Linear) return normalize(slerp(key(s.i), key(s.i + 1), s.u));
    return normalize(Quat{cubic(ch, s, 0), cubic(ch, s, 1), cubic(ch, s, 2), cubic(ch, s, 3)});
}

void sample(const Clip& clip, float time, std::span<NodePose> pose) noexcept {
    const float t = std::clamp(time, 0.0f, std::max(clip.duration, 0.0f));
    for (const Channel& ch : clip.channels) {
        if (ch.node >= pose.size()) continue;
        NodePose& p = pose[ch.node];
        switch (ch.path) {
        case Path::Translation: p.translation = sample_vec3(ch, t); break;
        case Path::Rotation: p.rotation = sample_quat(ch, t); break;
        case Path::Scale: p.scale = sample_vec3(ch, t); break;
        case Path::Weights: break; // expanded into weight nodes by expand_morph_weights
        }
    }
}

NodePose blend(const NodePose& a, const NodePose& b, float w) noexcept {
    NodePose out;
    out.translation = lerp(a.translation, b.translation, w);
    out.scale = lerp(a.scale, b.scale, w);
    Quat q = b.rotation;
    if (a.rotation.x * q.x + a.rotation.y * q.y + a.rotation.z * q.z + a.rotation.w * q.w < 0.0f)
        q = {-q.x, -q.y, -q.z, -q.w};
    out.rotation = normalize(Quat{a.rotation.x + (q.x - a.rotation.x) * w, a.rotation.y + (q.y - a.rotation.y) * w,
                                  a.rotation.z + (q.z - a.rotation.z) * w, a.rotation.w + (q.w - a.rotation.w) * w});
    return out;
}

} // namespace victus::animation
