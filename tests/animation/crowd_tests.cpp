// SPDX-License-Identifier: Apache-2.0
// Crowd animation (ADR-0200, GAP-10): baked skin palettes match per-frame pose evaluation,
// frame lookup wraps and clamps, bad inputs are refused, agents advance and serialize.
#include "victus/animation/crowd.hpp"
#include "victus/core/json.hpp"

#include <doctest.h>

#include <cmath>
#include <numbers>

using namespace victus;
using namespace victus::animation;
using math::Mat4;
using math::Quat;
using math::Vec3;

namespace {

bool near(const Mat4& a, const Mat4& b, float eps = 1e-4f) {
    for (int c = 0; c < 4; ++c) {
        const math::Vec4 x = a.c[c], y = b.c[c];
        if (std::abs(x.x - y.x) > eps || std::abs(x.y - y.y) > eps || std::abs(x.z - y.z) > eps || std::abs(x.w - y.w) > eps)
            return false;
    }
    return true;
}

// Root (scaled 2x, at x = 1) -> Hip -> Spine (1 up); clip "Bend" turns Spine 90 degrees about Z over 1 s.
struct Rig {
    std::vector<AnimationSet::Node> nodes;
    std::vector<Clip> clips;
    std::vector<std::uint32_t> joints{1, 2};
    std::vector<Mat4> inverse_bind;
    std::vector<math::Aabb> joint_bounds;
    Rig() {
        nodes.push_back({.name = "Root", .parent = -1, .rest = {{1, 0, 0}, Quat{}, {2, 2, 2}}});
        nodes.push_back({.name = "Hip", .parent = 0, .rest = {}});
        nodes.push_back({.name = "Spine", .parent = 1, .rest = {{0, 1, 0}, Quat{}, {1, 1, 1}}});
        Channel ch{.node = 2, .path = Path::Rotation, .interpolation = Interpolation::Linear, .times = {0, 1}};
        for (const float a : {0.0f, std::numbers::pi_v<float> / 2}) {
            const Quat q = Quat::axis_angle({0, 0, 1}, a);
            ch.values.insert(ch.values.end(), {q.x, q.y, q.z, q.w});
        }
        clips.push_back({.name = "Bend", .duration = 1.0f, .channels = {ch}});
        clips.push_back({.name = "Still", .duration = 0.0f});
        // Bind = rest: inverse of each joint's rest world matrix.
        const std::vector<Mat4> world = worlds(rest());
        inverse_bind = {math::inverse(world[1]), math::inverse(world[2])};
        joint_bounds = {math::Aabb{{-0.2f, 0, -0.2f}, {0.2f, 1, 0.2f}}, math::Aabb{{-0.2f, 1, -0.2f}, {0.2f, 2, 0.2f}}};
    }
    std::vector<NodePose> rest() const {
        std::vector<NodePose> p;
        for (const auto& n : nodes) p.push_back(n.rest);
        return p;
    }
    std::vector<Mat4> worlds(const std::vector<NodePose>& p) const {
        std::vector<Mat4> w(nodes.size());
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const Mat4 local = Mat4::trs(p[i].translation, p[i].rotation, p[i].scale);
            w[i] = nodes[i].parent >= 0 ? w[static_cast<std::size_t>(nodes[i].parent)] * local : local;
        }
        return w;
    }
    CrowdBakeInput input() const {
        CrowdBakeInput in;
        in.nodes = nodes;
        in.clips = clips;
        in.joints = joints;
        in.inverse_bind = inverse_bind;
        in.joint_bounds = joint_bounds;
        return in;
    }
};

} // namespace

TEST_CASE("crowd bake: palettes equal per-frame pose evaluation") {
    const Rig rig;
    auto bake = bake_crowd(rig.input());
    REQUIRE(bake.ok());
    CHECK(bake->joints == 2);
    REQUIRE(bake->clips.size() == 2);
    const BakedClip& bend = bake->clips[0];
    CHECK(bend.first_frame == 1);
    CHECK(bend.frames == 31); // 30 fps over 1 s plus the last key
    CHECK(bake->clips[1].frames == 1);
    CHECK(bake->total_frames() == 1 + 31 + 1);
    CHECK(bake->bytes() == bake->total_frames() * 2 * sizeof(Mat4));
    // Frame 0 is the bind pose: identity skin matrices.
    for (std::uint32_t j = 0; j < 2; ++j) CHECK(near(bake->matrices[j], Mat4{}));
    for (const std::uint32_t k : {0u, 7u, 15u, 30u}) {
        const float t = static_cast<float>(k) / 30.0f;
        std::vector<NodePose> pose = rig.rest();
        sample(rig.clips[0], t, pose);
        const std::vector<Mat4> w = rig.worlds(pose);
        for (std::uint32_t j = 0; j < 2; ++j) {
            const Mat4 expect = w[rig.joints[j]] * rig.inverse_bind[j];
            CHECK(near(bake->matrices[(bend.first_frame + k) * 2 + j], expect));
        }
    }
    // Rotating about the scaled, offset Spine swings the upper segment to +x: the clip bounds grow, the rest bounds do not.
    CHECK(bend.bounds.max.x > bake->rest_bounds.max.x + 0.5f);
    CHECK(bake->rest_bounds.valid());
    CHECK(bake->find_clip("Bend") == 0);
    CHECK(bake->find_clip("Run") == -1);
}

TEST_CASE("crowd bake: frame lookup wraps, clamps and snaps") {
    const Rig rig;
    auto bake = bake_crowd(rig.input());
    REQUIRE(bake.ok());
    CHECK(bake->frame_at(0, 0.0f, true) == 1);
    CHECK(bake->frame_at(0, 0.5f, true) == 16);
    CHECK(bake->frame_at(0, 1.5f, true) == 16);  // wrapped
    CHECK(bake->frame_at(0, -0.5f, true) == 16); // negative times wrap too
    CHECK(bake->frame_at(0, 1.0f, true) == 1);   // the loop's end is its start
    CHECK(bake->frame_at(0, 5.0f, false) == 31); // clamped at the last key
    CHECK(bake->frame_at(0, -1.0f, false) == 1);
    CHECK(bake->frame_at(0, std::nanf(""), true) == 1);
    CHECK(bake->frame_at(1, 3.0f, true) == 32); // zero-length clip: its only frame
    CHECK(bake->frame_at(9, 0.5f, true) == 0);  // unknown clip: bind pose
}

TEST_CASE("crowd bake: invalid inputs and oversized bakes are refused") {
    const Rig rig;
    CrowdBakeInput in = rig.input();
    in.joints = {};
    CHECK_FALSE(bake_crowd(in).ok());
    in = rig.input();
    in.inverse_bind = std::span<const Mat4>(rig.inverse_bind.data(), 1);
    CHECK_FALSE(bake_crowd(in).ok());
    in = rig.input();
    const std::vector<std::uint32_t> bad{1, 9};
    in.joints = bad;
    CHECK_FALSE(bake_crowd(in).ok());
    in = rig.input();
    in.fps = 0;
    CHECK_FALSE(bake_crowd(in).ok());
    in = rig.input();
    in.max_bytes = 1024;
    auto big = bake_crowd(in);
    REQUIRE_FALSE(big.ok());
    CHECK(big.error().message.find("limit") != std::string::npos);
    // Long clips are sampled more sparsely instead of growing without bound.
    in = rig.input();
    in.max_frames_per_clip = 5;
    auto sparse = bake_crowd(in);
    REQUIRE(sparse.ok());
    CHECK(sparse->clips[0].frames == 5);
    CHECK(sparse->frame_at(0, 0.5f, false) == 3);
    // No clips: only the bind pose.
    in = rig.input();
    in.clips = {};
    auto rest = bake_crowd(in);
    REQUIRE(rest.ok());
    CHECK(rest->total_frames() == 1);
}

TEST_CASE("crowd agents advance and serialize") {
    register_components();
    scene::Scene s;
    const scene::Entity a = s.create("Agent");
    auto& ca = s.world().add<CrowdAgentComponent>(a);
    ca.model = Uuid::generate();
    ca.clip = "Walk";
    ca.speed = 2.0f;
    ca.skin = 1;
    ca.tint = {0.5f, 0.25f, 1, 1};
    ca.cast_shadows = false;
    const scene::Entity b = s.create("Idle");
    s.world().add<CrowdAgentComponent>(b).playing = false;
    const CrowdStats st = update_crowd(s.world(), 0.25f);
    CHECK(st.agents == 2);
    CHECK(st.advanced == 1);
    CHECK(s.world().get<CrowdAgentComponent>(a)->time == doctest::Approx(0.5f));
    CHECK(s.world().get<CrowdAgentComponent>(b)->time == 0.0f);

    AnimationSystem sys;
    sys.update(s, 0.25f); // the animation system advances crowds too
    CHECK(s.world().get<CrowdAgentComponent>(a)->time == doctest::Approx(1.0f));

    const std::string text = json::stringify(s.save());
    scene::Scene loaded;
    REQUIRE(loaded.load(json::parse(text).value()).ok());
    const auto* la = loaded.world().get<CrowdAgentComponent>(loaded.find_by_name("Agent"));
    REQUIRE(la);
    CHECK(la->model == ca.model);
    CHECK(la->clip == "Walk");
    CHECK(la->skin == 1);
    CHECK(la->speed == doctest::Approx(2.0f));
    CHECK(la->time == doctest::Approx(1.0f));
    CHECK(la->tint.g == doctest::Approx(0.25f));
    CHECK_FALSE(la->cast_shadows);
    CHECK_FALSE(loaded.world().get<CrowdAgentComponent>(loaded.find_by_name("Idle"))->playing);
}
