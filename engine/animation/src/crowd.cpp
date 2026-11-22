// SPDX-License-Identifier: Apache-2.0
#include "victus/animation/crowd.hpp"

#include "victus/scene/reflect.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>

namespace victus::animation {

using namespace math;

std::int32_t CrowdBake::find_clip(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < clips.size(); ++i)
        if (clips[i].name == name) return static_cast<std::int32_t>(i);
    return -1;
}

std::uint32_t CrowdBake::frame_at(std::uint32_t clip, float time, bool loop) const noexcept {
    if (clip >= clips.size()) return 0;
    const BakedClip& c = clips[clip];
    if (c.frames <= 1 || !(c.duration > 0.0f) || !std::isfinite(time)) return c.first_frame;
    float t = time;
    if (loop) {
        t = std::fmod(t, c.duration);
        if (t < 0.0f) t += c.duration;
    } else {
        t = std::clamp(t, 0.0f, c.duration);
    }
    // Samples are spread evenly over [0, duration] (the last one is the last key).
    const float step = c.duration / static_cast<float>(c.frames - 1);
    auto k = static_cast<std::uint32_t>(std::lround(t / step));
    if (loop && k >= c.frames - 1) k = 0; // the end of a loop is its start
    return c.first_frame + std::min(k, c.frames - 1);
}

Result<CrowdBake> bake_crowd(const CrowdBakeInput& in) {
    if (in.joints.empty()) return make_error(ErrorCode::InvalidArgument, "crowd bake: the skin has no joints");
    if (in.inverse_bind.size() != in.joints.size())
        return make_error(ErrorCode::InvalidArgument,
                          std::format("crowd bake: {} inverse bind matrices for {} joints", in.inverse_bind.size(), in.joints.size()));
    if (!(in.fps > 0.0f) || !std::isfinite(in.fps)) return make_error(ErrorCode::InvalidArgument, "crowd bake: fps must be positive");
    for (const std::uint32_t j : in.joints)
        if (j >= in.nodes.size()) return make_error(ErrorCode::InvalidArgument, std::format("crowd bake: joint node {} out of range", j));
    for (std::size_t i = 0; i < in.nodes.size(); ++i)
        if (in.nodes[i].parent >= static_cast<std::int32_t>(i))
            return make_error(ErrorCode::InvalidArgument, std::format("crowd bake: node {} comes before its parent", i));

    CrowdBake bake;
    bake.fps = in.fps;
    bake.joints = static_cast<std::uint32_t>(in.joints.size());
    const std::uint32_t cap = std::max(in.max_frames_per_clip, 2u);
    std::size_t total = 1; // the bind pose
    for (const Clip& c : in.clips) {
        const float d = std::isfinite(c.duration) ? std::max(c.duration, 0.0f) : 0.0f;
        const auto n = d > 0.0f ? std::min<std::uint64_t>(static_cast<std::uint64_t>(std::ceil(d * in.fps)) + 1, cap) : 1;
        total += n;
    }
    const std::size_t bytes = total * in.joints.size() * sizeof(Mat4);
    if (bytes > in.max_bytes)
        return make_error(ErrorCode::InvalidArgument,
                          std::format("crowd bake: {:.1f} MB exceeds the {:.1f} MB limit ({} frames x {} joints)", bytes / 1048576.0,
                                      in.max_bytes / 1048576.0, total, in.joints.size()));
    bake.matrices.reserve(total * in.joints.size());

    std::vector<NodePose> rest(in.nodes.size()), pose(in.nodes.size());
    for (std::size_t i = 0; i < in.nodes.size(); ++i) rest[i] = in.nodes[i].rest;
    std::vector<Mat4> world(in.nodes.size());
    auto emit = [&](std::span<const NodePose> p, Aabb* bounds) {
        for (std::size_t i = 0; i < in.nodes.size(); ++i) {
            const Mat4 local = Mat4::trs(p[i].translation, p[i].rotation, p[i].scale);
            const std::int32_t parent = in.nodes[i].parent;
            world[i] = parent >= 0 ? world[static_cast<std::size_t>(parent)] * local : local;
        }
        for (std::size_t j = 0; j < in.joints.size(); ++j) {
            const Mat4 skin = world[in.joints[j]] * in.inverse_bind[j];
            bake.matrices.push_back(skin);
            if (bounds && j < in.joint_bounds.size() && in.joint_bounds[j].valid()) {
                const Aabb b = in.joint_bounds[j].transformed(skin);
                bounds->expand(b.min);
                bounds->expand(b.max);
            }
        }
    };
    // Frame 0: the bind pose (agents whose clip is empty or unknown).
    emit(rest, &bake.rest_bounds);
    std::uint32_t frame = 1;
    for (const Clip& c : in.clips) {
        BakedClip bc;
        bc.name = c.name;
        bc.duration = std::isfinite(c.duration) ? std::max(c.duration, 0.0f) : 0.0f;
        bc.first_frame = frame;
        bc.frames = bc.duration > 0.0f
                        ? static_cast<std::uint32_t>(std::min<std::uint64_t>(static_cast<std::uint64_t>(std::ceil(bc.duration * in.fps)) + 1, cap))
                        : 1;
        for (std::uint32_t k = 0; k < bc.frames; ++k) {
            const float t = bc.frames > 1 ? bc.duration * static_cast<float>(k) / static_cast<float>(bc.frames - 1) : 0.0f;
            std::copy(rest.begin(), rest.end(), pose.begin()); // nodes a clip does not animate keep their rest pose
            sample(c, t, pose);
            emit(pose, &bc.bounds);
        }
        frame += bc.frames;
        bake.clips.push_back(std::move(bc));
    }
    return bake;
}

void register_crowd_components() {
    static std::once_flag once;
    std::call_once(once, [] {
        scene::Schema<CrowdAgentComponent>("CrowdAgent", "Animation")
            .field(&CrowdAgentComponent::model, "model", {.tooltip = "Skinned model asset drawn from its baked clips", .hint = "model"})
            .field(&CrowdAgentComponent::skin, "skin", {.min = 0, .max = 1024})
            .field(&CrowdAgentComponent::clip, "clip", {.tooltip = "Clip to play; empty shows the bind pose"})
            .field(&CrowdAgentComponent::time, "time", {.min = -1e6f, .max = 1e6f, .speed = 0.01f, .tooltip = "seconds"})
            .field(&CrowdAgentComponent::speed, "speed", {.min = -10, .max = 10, .speed = 0.01f})
            .field(&CrowdAgentComponent::playing, "playing")
            .field(&CrowdAgentComponent::loop, "loop")
            .field(&CrowdAgentComponent::tint, "tint", {.tooltip = "Multiplies the model's base colours"})
            .field(&CrowdAgentComponent::cast_shadows, "cast_shadows")
            .commit();
    });
}

CrowdStats update_crowd(ecs::World& world, float dt) {
    CrowdStats st;
    world.each<CrowdAgentComponent>([&](scene::Entity, CrowdAgentComponent& a) {
        ++st.agents;
        if (!a.playing || a.speed == 0.0f) return;
        a.time += dt * a.speed;
        if (!std::isfinite(a.time)) a.time = 0.0f;
        ++st.advanced;
    });
    return st;
}

} // namespace victus::animation
