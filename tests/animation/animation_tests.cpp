// SPDX-License-Identifier: Apache-2.0
// Animation clip sampling, blending and Animator playback tests (Slice 12).
#include "victus/animation/animation.hpp"
#include "victus/core/jobs.hpp"
#include "victus/core/json.hpp"

#include <doctest.h>

#include <cmath>
#include <numbers>

using namespace victus;
using namespace victus::animation;
using math::Quat;
using math::Vec3;
using scene::Entity;
using scene::Scene;
using scene::Transform;

namespace {

bool near(float a, float b, float eps = 1e-4f) { return std::abs(a - b) <= eps; }
bool near(Vec3 a, Vec3 b, float eps = 1e-4f) { return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps); }
float qdot(Quat a, Quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
bool same_rotation(Quat a, Quat b, float eps = 1e-4f) { return std::abs(std::abs(qdot(a, b)) - 1.0f) <= eps; }

Channel translation_x(std::uint32_t node, Interpolation interp, std::vector<float> times, std::vector<float> xs) {
    Channel ch{.node = node, .path = Path::Translation, .interpolation = interp, .times = std::move(times)};
    for (const float x : xs) ch.values.insert(ch.values.end(), {x, 0, 0});
    return ch;
}

Channel rotation_y(std::uint32_t node, std::vector<float> times, std::vector<float> angles) {
    Channel ch{.node = node, .path = Path::Rotation, .interpolation = Interpolation::Linear, .times = std::move(times)};
    for (const float a : angles) {
        const Quat q = Quat::axis_angle({0, 1, 0}, a);
        ch.values.insert(ch.values.end(), {q.x, q.y, q.z, q.w});
    }
    return ch;
}

// Turn angle below pi so the shortest path is unambiguous.
constexpr float kTurn = 0.9f * std::numbers::pi_v<float>;

// Two-node chain "Hips" -> "Arm"; "Move" translates Hips 0..2 in x over 1 s, "Turn" rotates Arm.
std::shared_ptr<AnimationSet> make_set() {
    auto set = std::make_shared<AnimationSet>();
    set->nodes.push_back({.name = "Hips", .parent = -1, .rest = {.translation = {0, 1, 0}}});
    set->nodes.push_back({.name = "Arm", .parent = 0, .rest = {.translation = {0.5f, 0, 0}}});
    Clip move{.name = "Move", .duration = 1.0f};
    move.channels.push_back(translation_x(0, Interpolation::Linear, {0, 1}, {0, 2}));
    Clip turn{.name = "Turn", .duration = 2.0f};
    turn.channels.push_back(rotation_y(1, {0, 2}, {0, kTurn}));
    set->clips.push_back(move);
    set->clips.push_back(turn);
    return set;
}

struct Rig {
    Scene scene;
    Entity root, hips, arm;
    Uuid model = Uuid::generate();
    AnimationSystem system;
    Rig() {
        system.add_set(model, make_set());
        root = scene.create("Character");
        hips = scene.create("Hips", root);
        arm = scene.create("Arm", hips);
        auto& a = scene.world().add<AnimatorComponent>(root);
        a.model = model;
        a.clip = "Move";
    }
    Transform& t(Entity e) { return *scene.world().get<Transform>(e); }
};

} // namespace

TEST_CASE("channel sampling: step, linear and cubic spline") {
    const Channel step = translation_x(0, Interpolation::Step, {0, 1, 2}, {0, 10, 20});
    CHECK(near(sample_vec3(step, 0.5f).x, 0));
    CHECK(near(sample_vec3(step, 1.0f).x, 10));
    CHECK(near(sample_vec3(step, 1.99f).x, 10));
    CHECK(near(sample_vec3(step, 5.0f).x, 20)); // clamps past the end
    CHECK(near(sample_vec3(step, -1.0f).x, 0));

    const Channel lin = translation_x(0, Interpolation::Linear, {0, 1, 3}, {0, 10, 30});
    CHECK(near(sample_vec3(lin, 0.25f).x, 2.5f));
    CHECK(near(sample_vec3(lin, 2.0f).x, 20));

    // Cubic: values 0 -> 1 with zero tangents gives smoothstep: p(0.5) = 0.5, p(0.25) = 0.15625.
    Channel cub{.node = 0, .path = Path::Translation, .interpolation = Interpolation::CubicSpline, .times = {0, 1}};
    cub.values = {0, 0, 0, 0, 0, 0, 0, 0, 0, /*key1*/ 0, 0, 0, 1, 0, 0, 0, 0, 0};
    CHECK(near(sample_vec3(cub, 0.5f).x, 0.5f));
    CHECK(near(sample_vec3(cub, 0.25f).x, 0.15625f));
    // Tangent of 1 at both ends with dt = 1 is the straight line.
    cub.values = {0, 0, 0, 0, 0, 0, 1, 0, 0, /*key1*/ 1, 0, 0, 1, 0, 0, 0, 0, 0};
    CHECK(near(sample_vec3(cub, 0.3f).x, 0.3f));
    CHECK(validate(Clip{.name = "c", .duration = 1, .channels = {cub}}, 1).ok());
}

TEST_CASE("rotation sampling is normalized and takes the shortest path") {
    const Channel ch = rotation_y(0, {0, 1}, {0, std::numbers::pi_v<float> / 2});
    const Quat mid = sample_quat(ch, 0.5f);
    CHECK(same_rotation(mid, Quat::axis_angle({0, 1, 0}, std::numbers::pi_v<float> / 4)));
    CHECK(near(qdot(mid, mid), 1.0f));
    // Keys with opposite signs (same orientation) must not spin the long way round.
    Channel flip{.node = 0, .path = Path::Rotation, .interpolation = Interpolation::Linear, .times = {0, 1}};
    flip.values = {0, 0, 0, 1, 0, 0, 0, -1};
    CHECK(same_rotation(sample_quat(flip, 0.5f), Quat{}));
    // Blend sign correction as well.
    const NodePose a{.rotation = Quat{0, 0, 0, 1}}, b{.rotation = Quat{0, 0, 0, -1}};
    CHECK(same_rotation(blend(a, b, 0.5f).rotation, Quat{}));
}

TEST_CASE("clip validation rejects corrupt channels") {
    Clip c{.name = "bad", .duration = 1};
    c.channels.push_back(translation_x(0, Interpolation::Linear, {0, 1}, {0, 1}));
    CHECK(validate(c, 1).ok());
    CHECK_FALSE(validate(c, 0).ok()); // node out of range
    c.channels[0].times = {1, 0};
    CHECK_FALSE(validate(c, 1).ok()); // unsorted
    c.channels[0].times = {0, 1};
    c.channels[0].values.pop_back();
    CHECK_FALSE(validate(c, 1).ok()); // count mismatch
    c.channels[0].values.push_back(std::nanf(""));
    CHECK_FALSE(validate(c, 1).ok()); // non-finite
    c.channels[0] = translation_x(0, Interpolation::CubicSpline, {0, 1}, {0, 1}); // cubic needs 3x values
    CHECK_FALSE(validate(c, 1).ok());
}

TEST_CASE("keyframe reduction keeps the curve within tolerance") {
    // Translation: a straight ramp, then a sine. The ramp collapses to its end keys.
    Channel t;
    t.path = Path::Translation;
    for (int k = 0; k <= 120; ++k) {
        const float time = static_cast<float>(k) / 30.0f;
        t.times.push_back(time);
        const float y = k <= 60 ? time : 2.0f + 0.5f * std::sin((time - 2.0f) * 3.0f);
        t.values.insert(t.values.end(), {1.0f, y, 0.0f});
    }
    const Channel original = t;
    const KeyframeReduction settings;
    const std::size_t removed = reduce_keyframes(t, settings);
    CHECK(removed > 60);
    CHECK(t.times.size() + removed == original.times.size());
    CHECK(t.times.front() == original.times.front());
    CHECK(t.times.back() == original.times.back());
    REQUIRE(validate(Clip{.name = "c", .duration = 4, .channels = {t}}, 1).ok());
    float worst = 0;
    for (int i = 0; i <= 400; ++i) {
        const float time = 4.0f * static_cast<float>(i) / 400.0f;
        worst = std::max(worst, length(sample_vec3(t, time) - sample_vec3(original, time)));
    }
    CHECK(worst <= 0.0025f + 1e-5f); // 0.1% of the largest value (2.5); only y differs

    // Rotation: a constant-speed turn about Y is exactly reproduced by slerp of its end keys.
    Channel r;
    r.path = Path::Rotation;
    for (int k = 0; k <= 30; ++k) {
        r.times.push_back(static_cast<float>(k) / 30.0f);
        const Quat q = math::Quat::axis_angle({0, 1, 0}, static_cast<float>(k) * 0.05f);
        r.values.insert(r.values.end(), {q.x, q.y, q.z, q.w});
    }
    // A wiggle in the middle must survive.
    const Quat bump = math::Quat::axis_angle({1, 0, 0}, 0.2f);
    r.values[15 * 4 + 0] = bump.x, r.values[15 * 4 + 1] = bump.y, r.values[15 * 4 + 2] = bump.z, r.values[15 * 4 + 3] = bump.w;
    const Channel rot = r;
    reduce_keyframes(r, settings);
    CHECK(r.times.size() < rot.times.size());
    CHECK(std::find(r.times.begin(), r.times.end(), rot.times[15]) != r.times.end());
    for (std::size_t k = 0; k < rot.times.size(); ++k) {
        const Quat a = sample_quat(r, rot.times[k]), b = sample_quat(rot, rot.times[k]);
        CHECK(std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) >= std::cos(0.05f * std::numbers::pi_v<float> / 360.0f) - 1e-6f);
    }

    // Step keys: only value changes stay. Cubic splines and short channels are untouched.
    Channel s;
    s.path = Path::Scale;
    s.interpolation = Interpolation::Step;
    for (int k = 0; k < 6; ++k) {
        s.times.push_back(static_cast<float>(k));
        const float v = k < 3 ? 1.0f : 2.0f;
        s.values.insert(s.values.end(), {v, v, v});
    }
    CHECK(reduce_keyframes(s, settings) == 3); // keeps 0, 3 and the last key
    CHECK(s.times == std::vector<float>{0, 3, 5});
    Channel c = original;
    c.interpolation = Interpolation::CubicSpline;
    CHECK(reduce_keyframes(c, settings) == 0);
    Channel two;
    two.times = {0, 1};
    two.values = {0, 0, 0, 1, 1, 1};
    CHECK(reduce_keyframes(two, settings) == 0);
}

TEST_CASE("wrap_time loops and clamps") {
    CHECK(near(wrap_time(2.5f, 1.0f, true), 0.5f));
    CHECK(near(wrap_time(-0.25f, 1.0f, true), 0.75f));
    CHECK(near(wrap_time(2.5f, 1.0f, false), 1.0f));
    CHECK(near(wrap_time(3.0f, 0.0f, true), 0.0f));
}

TEST_CASE("animator binds nodes by name and drives transforms") {
    Rig rig;
    rig.system.update(rig.scene, 0.25f);
    CHECK(near(rig.t(rig.hips).position, {0.5f, 0, 0}));
    CHECK(near(rig.t(rig.arm).position, {0, 0, 0})); // untouched (not animated by Move)
    const AnimatorStatus st = rig.system.status(rig.scene, rig.root);
    CHECK(st.valid);
    CHECK(st.bound_nodes == 2);
    CHECK(st.animated_nodes == 1);
    CHECK(near(st.duration, 1.0f));
    CHECK(rig.system.stats().animators == 1);
    CHECK(rig.system.stats().nodes_written == 1);

    // Looping wraps; non-looping clamps and reports finished.
    rig.system.update(rig.scene, 1.0f);
    CHECK(near(rig.scene.world().get<AnimatorComponent>(rig.root)->time, 0.25f));
    rig.scene.world().get<AnimatorComponent>(rig.root)->loop = false;
    rig.system.update(rig.scene, 5.0f);
    CHECK(near(rig.t(rig.hips).position, {2, 0, 0}));
    CHECK(rig.system.status(rig.scene, rig.root).finished);

    // Paused animators hold their pose but still apply edits of `time` (editor scrubbing).
    auto* a = rig.scene.world().get<AnimatorComponent>(rig.root);
    a->playing = false;
    a->time = 0.5f;
    rig.system.update(rig.scene, 1.0f);
    CHECK(near(rig.t(rig.hips).position, {1, 0, 0}));

    // Negative speed plays backwards.
    a->playing = true;
    a->loop = true;
    a->speed = -1.0f;
    rig.system.update(rig.scene, 0.25f);
    CHECK(near(rig.t(rig.hips).position, {0.5f, 0, 0}));
}

TEST_CASE("clip changes cross-fade and play() switches clips") {
    Rig rig;
    rig.system.update(rig.scene, 0.5f); // Hips x = 1
    REQUIRE(rig.system.play(rig.scene, rig.root, "Turn", 0.5f));
    CHECK_FALSE(rig.system.play(rig.scene, rig.root, "Nope"));
    CHECK_FALSE(rig.system.play(rig.scene, rig.hips, "Turn")); // no animator

    rig.system.update(rig.scene, 0.25f); // halfway through the fade
    const AnimatorStatus st = rig.system.status(rig.scene, rig.root);
    CHECK(st.clip == "Turn");
    CHECK(near(st.blend, 0.5f));
    CHECK(st.animated_nodes == 2);
    // Hips: Move at t = 0.75 (x = 1.5) fading to Turn's rest pose (x = 0): 0.75.
    CHECK(near(rig.t(rig.hips).position, {0.75f, 0.5f, 0}));
    // Arm: rest (identity) blended halfway with Turn at t = 0.25 (kTurn/8): ~kTurn/16.
    CHECK(same_rotation(rig.t(rig.arm).rotation, Quat::axis_angle({0, 1, 0}, kTurn / 16), 1e-4f));

    rig.system.update(rig.scene, 0.5f); // fade complete
    CHECK(near(rig.system.status(rig.scene, rig.root).blend, 1.0f));
    CHECK(rig.system.status(rig.scene, rig.root).animated_nodes == 1);
    CHECK(same_rotation(rig.t(rig.arm).rotation, Quat::axis_angle({0, 1, 0}, kTurn * 0.375f), 1e-4f));

    // Editing the component's clip (inspector, serialized scenes) also switches without play().
    rig.scene.world().get<AnimatorComponent>(rig.root)->crossfade = 0.0f;
    rig.scene.world().get<AnimatorComponent>(rig.root)->clip = "Move";
    rig.system.update(rig.scene, 0.25f);
    CHECK(near(rig.t(rig.hips).position, {0.5f, 0, 0}));
    const auto names = rig.system.clips(rig.scene, rig.root);
    REQUIRE(names.size() == 2);
    CHECK(names[1] == "Turn");
}

TEST_CASE("animator rebinds after the hierarchy changes and survives missing models") {
    Rig rig;
    rig.system.update(rig.scene, 0.25f);
    rig.scene.destroy(rig.hips); // also destroys Arm
    rig.system.update(rig.scene, 0.25f);
    CHECK(rig.system.status(rig.scene, rig.root).bound_nodes == 0);
    const Entity hips2 = rig.scene.create("Hips", rig.root);
    rig.system.update(rig.scene, 0.25f);
    CHECK(rig.system.status(rig.scene, rig.root).bound_nodes == 1);
    CHECK(near(rig.scene.world().get<Transform>(hips2)->position.x, 1.5f));

    // Unknown model: counted, nothing written, no crash.
    const Entity other = rig.scene.create("Other");
    rig.scene.world().add<AnimatorComponent>(other).model = Uuid::generate();
    rig.system.update(rig.scene, 0.1f);
    CHECK(rig.system.stats().missing_models == 1);
    CHECK_FALSE(rig.system.status(rig.scene, other).valid);

    // Removing the component drops its state.
    rig.scene.world().remove<AnimatorComponent>(rig.root);
    rig.system.update(rig.scene, 0.1f);
    CHECK(rig.system.stats().animators == 1);

    // Duplicated characters bind to their own nodes, not the original's.
    Rig r2;
    r2.system.update(r2.scene, 0.25f);
    const Entity copy = r2.scene.duplicate(r2.root);
    r2.system.update(r2.scene, 0.25f);
    const Entity copy_hips = r2.scene.children(copy).at(0);
    CHECK(near(r2.scene.world().get<Transform>(copy_hips)->position.x, r2.t(r2.hips).position.x));
    CHECK(r2.system.stats().nodes_written == 2);
}

TEST_CASE("provider supplies sets and invalid clips are rejected") {
    int calls = 0;
    const Uuid good = Uuid::generate(), bad = Uuid::generate();
    AnimationSystem sys({.provider = [&](const Uuid& id) -> std::shared_ptr<const AnimationSet> {
        ++calls;
        if (id == good) return make_set();
        auto s = make_set();
        s->clips[0].channels[0].node = 7; // out of range
        return s;
    }});
    CHECK(sys.set(good) != nullptr);
    CHECK(sys.set(good) != nullptr);
    CHECK(calls == 1); // cached
    CHECK(sys.set(bad) == nullptr);
    sys.invalidate(good);
    CHECK(sys.set(good) != nullptr);
    CHECK(calls == 3);
}

TEST_CASE("animators evaluate in parallel with identical results") {
    jobs::JobSystem js(2);
    const Uuid model = Uuid::generate();
    AnimationSystem serial, parallel({.jobs = &js, .parallel_threshold = 4});
    serial.add_set(model, make_set());
    parallel.add_set(model, make_set());
    Scene a, b;
    std::vector<Entity> ha, hb;
    for (int i = 0; i < 64; ++i) {
        for (auto [scene, hips] : {std::pair{&a, &ha}, std::pair{&b, &hb}}) {
            const Entity r = scene->create("C");
            hips->push_back(scene->create("Hips", r));
            scene->create("Arm", hips->back());
            auto& anim = scene->world().add<AnimatorComponent>(r);
            anim.model = model;
            anim.clip = i % 2 ? "Move" : "Turn";
            anim.speed = 0.5f + static_cast<float>(i) * 0.01f;
        }
    }
    for (int f = 0; f < 10; ++f) {
        serial.update(a, 1.0f / 30.0f);
        parallel.update(b, 1.0f / 30.0f);
    }
    for (std::size_t i = 0; i < ha.size(); ++i)
        CHECK(near(a.world().get<Transform>(ha[i])->position, b.world().get<Transform>(hb[i])->position, 0));
    CHECK(parallel.stats().nodes_written == serial.stats().nodes_written);
    CHECK(parallel.stats().animators == 64);
}

TEST_CASE("animation components serialize and bind to the scene") {
    register_components();
    Scene s;
    const Entity e = s.create("Char");
    auto& a = s.world().add<AnimatorComponent>(e);
    a.model = Uuid::generate();
    a.clip = "Walk";
    a.speed = 1.5f;
    s.world().add<SkinnedMeshComponent>(e).skin = 3;
    const std::string text = json::stringify(s.save());
    Scene loaded;
    REQUIRE(loaded.load(json::parse(text).value()).ok());
    const Entity le = loaded.find_by_name("Char");
    const auto* la = loaded.world().get<AnimatorComponent>(le);
    REQUIRE(la);
    CHECK(la->clip == "Walk");
    CHECK(la->model == a.model);
    CHECK(near(la->speed, 1.5f));
    CHECK(loaded.world().get<SkinnedMeshComponent>(le)->skin == 3);

    AnimationSystem sys;
    gameplay::Simulation sim;
    add_animation_systems(sim, sys);
    CHECK(system_of(s) == nullptr);
    sim.tick(s, 1.0f / 60.0f);
    CHECK(system_of(s) == &sys);
}

TEST_CASE("skeleton lines connect bound parents and children") {
    Rig rig;
    rig.system.update(rig.scene, 0.0f);
    rig.scene.update_transforms();
    int lines = 0;
    skeleton_lines(rig.scene, [&](Vec3 from, Vec3 to) {
        ++lines;
        CHECK(near(from, {0, 0, 0}));
        CHECK(near(to, {0, 0, 0}));
    });
    CHECK(lines == 1);
}
