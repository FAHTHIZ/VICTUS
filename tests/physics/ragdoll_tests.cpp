// SPDX-License-Identifier: Apache-2.0
// Articulated ragdoll tests (A.R.C. stage 1, ADR-0091): a 16-bone humanoid built from scene
// entities, simulated with the real Jolt backend through the gameplay phases (FixedUpdate step,
// PostUpdate pose write-back) at 60 Hz.
#include "victus/physics/physics.hpp"
#include "victus/scene/reflect.hpp"

#include <doctest.h>

#include <cmath>
#include <map>
#include <string>

using namespace victus;
using namespace victus::physics;
using scene::Scene;
using scene::Transform;
using Mode = RagdollComponent::Mode;

namespace {

constexpr float kDt = 1.0f / 60.0f;

std::unique_ptr<PhysicsWorld> make_world() {
    auto w = PhysicsWorld::create({});
    REQUIRE(w.ok());
    return std::move(w.value());
}

void ground(Scene& scene) {
    const Entity e = scene.create("Ground");
    scene.world().get<Transform>(e)->position = {0, -0.5f, 0};
    ColliderComponent c;
    c.size = {40, 1, 40};
    scene.world().add<ColliderComponent>(e, c);
}

struct Rig {
    Entity root;
    std::map<std::string, Entity> bones;
    Entity operator[](const std::string& n) const { return bones.at(n); }
};

// A T-posed humanoid, hips 1 m above the root. Shoulder.* are plain (non-bone) entities between
// the chest and the upper arms.
Rig humanoid(Scene& scene, math::Vec3 at = {}) {
    Rig r;
    r.root = scene.create("Character");
    scene.world().get<Transform>(r.root)->position = at;
    auto bone = [&](const std::string& name, const std::string& parent, math::Vec3 local) {
        const Entity p = parent.empty() ? r.root : r.bones.at(parent);
        const Entity e = scene.create(name, p);
        scene.world().get<Transform>(e)->position = local;
        r.bones[name] = e;
    };
    bone("Hips", "", {0, 1.0f, 0});
    bone("Spine", "Hips", {0, 0.15f, 0});
    bone("Chest", "Spine", {0, 0.2f, 0});
    bone("Neck", "Chest", {0, 0.25f, 0});
    bone("Head", "Neck", {0, 0.08f, 0});
    for (const char* side : {"L", "R"}) {
        const float s = std::string(side) == "L" ? 1.0f : -1.0f;
        const std::string t = std::string(".") + side;
        bone("Shoulder" + t, "Chest", {s * 0.05f, 0.18f, 0});
        bone("UpperArm" + t, "Shoulder" + t, {s * 0.12f, 0, 0});
        bone("LowerArm" + t, "UpperArm" + t, {s * 0.28f, 0, 0});
        bone("Hand" + t, "LowerArm" + t, {s * 0.25f, 0, 0});
        bone("UpperLeg" + t, "Hips", {s * 0.1f, -0.05f, 0});
        bone("LowerLeg" + t, "UpperLeg" + t, {0, -0.43f, 0});
        bone("Foot" + t, "LowerLeg" + t, {0, -0.42f, 0});
    }
    return r;
}

math::Vec3 world_pos(const Scene& scene, Entity e) { return scene.world_matrix(e).transform_point({}); }

bool finite(math::Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

struct Sim {
    Scene scene;
    std::unique_ptr<PhysicsWorld> world = make_world();
    float dt;
    gameplay::Simulation sim;
    explicit Sim(float fixed = kDt) : dt(fixed), sim(fixed) { add_physics_systems(sim, *world); }
    void run(float seconds) {
        for (int i = 0; i < static_cast<int>(seconds / dt + 0.5f); ++i) sim.tick(scene, dt);
    }
};

const char* const kSkeleton[] = {"Hips", "Spine", "Chest", "Head", "UpperArm.L", "LowerArm.L", "Hand.L", "UpperArm.R",
                                 "LowerArm.R", "Hand.R", "UpperLeg.L", "LowerLeg.L", "Foot.L", "UpperLeg.R", "LowerLeg.R", "Foot.R"};

} // namespace

TEST_CASE("make_humanoid_ragdoll recognises the humanoid bones and skips helpers") {
    register_components();
    Scene scene;
    const Rig r = humanoid(scene);
    CHECK(make_humanoid_ragdoll(scene, r.root) == 16);
    for (const char* n : kSkeleton) CHECK_MESSAGE(scene.world().get<RagdollBoneComponent>(r[n]) != nullptr, n);
    CHECK(scene.world().get<RagdollBoneComponent>(r["Neck"]) == nullptr);
    CHECK(scene.world().get<RagdollBoneComponent>(r["Shoulder.L"]) == nullptr);
    CHECK(scene.world().get<RagdollBoneComponent>(r["Head"])->shape == RagdollBoneComponent::Shape::Sphere);
    CHECK(scene.world().get<RagdollBoneComponent>(r["LowerLeg.L"])->swing_plane < 10.0f); // knees bend one way
    REQUIRE(scene.world().get<RagdollComponent>(r.root) != nullptr);
    CHECK(scene.world().get<RagdollComponent>(r.root)->mode == Mode::Off);

    // Common naming schemes.
    Scene other;
    const Entity root = other.create("Rig");
    const Entity hips = other.create("mixamorig:Hips", root);
    const Entity thigh = other.create("mixamorig:LeftUpLeg", hips);
    const Entity calf = other.create("mixamorig:LeftLeg", thigh);
    const Entity ik = other.create("ik_foot_l", root);
    const Entity unreal = other.create("thigh_r", hips);
    CHECK(make_humanoid_ragdoll(other, root) == 4);
    CHECK(other.world().get<RagdollBoneComponent>(calf) != nullptr);
    CHECK(other.world().get<RagdollBoneComponent>(unreal) != nullptr);
    CHECK(other.world().get<RagdollBoneComponent>(ik) == nullptr);

    Scene none;
    CHECK(make_humanoid_ragdoll(none, none.create("Prop")) == 0);
}

TEST_CASE("Ragdoll and RagdollBone round-trip through their schemas") {
    register_components();
    const auto* rs = scene::find_schema("Ragdoll");
    const auto* bs = scene::find_schema("RagdollBone");
    REQUIRE(rs);
    REQUIRE(bs);
    RagdollComponent r;
    r.mode = Mode::Powered;
    r.blend = 0.25f;
    r.pin_root = true;
    RagdollBoneComponent b;
    b.shape = RagdollBoneComponent::Shape::Box;
    b.twist_min = -12;
    b.strength = 0.5f;
    RagdollComponent r2;
    RagdollBoneComponent b2;
    REQUIRE(rs->load(&r2, rs->save(&r)).ok());
    REQUIRE(bs->load(&b2, bs->save(&b)).ok());
    CHECK(r2 == r);
    CHECK(b2 == b);
}

TEST_CASE("a passive ragdoll falls, keeps its joints together and comes to rest on the ground") {
    register_components();
    Sim s;
    ground(s.scene);
    const Rig r = humanoid(s.scene);
    REQUIRE(make_humanoid_ragdoll(s.scene, r.root) == 16);
    s.scene.world().get<RagdollComponent>(r.root)->mode = Mode::Passive;
    // Bone-to-parent distances are the joint lengths the constraints must preserve.
    std::map<std::string, float> lengths;
    const std::pair<const char*, const char*> links[] = {{"Spine", "Hips"},          {"Chest", "Spine"},
                                                         {"LowerArm.L", "UpperArm.L"}, {"Hand.L", "LowerArm.L"},
                                                         {"LowerLeg.R", "UpperLeg.R"}, {"Foot.R", "LowerLeg.R"}};
    for (const auto& [a, b] : links) lengths[a] = math::length(world_pos(s.scene, r[a]) - world_pos(s.scene, r[b]));

    s.run(0.1f);
    const RagdollState st = s.world->ragdoll_state(r.root);
    REQUIRE(st.valid);
    CHECK(st.bodies == 16);
    CHECK(st.mode == Mode::Passive);
    CHECK(s.world->has_body(r["Head"]));
    CHECK(s.world->ragdoll_of(r["Head"]) == r.root);

    s.run(4.0f);
    for (const char* n : kSkeleton) {
        const math::Vec3 p = world_pos(s.scene, r[n]);
        CHECK_MESSAGE(finite(p), n);
        CHECK_MESSAGE(p.y > -0.05f, n); // nothing fell through the ground
        CHECK_MESSAGE(p.y < 0.6f, n);   // everything collapsed
    }
    for (const auto& [a, b] : links)
        CHECK_MESSAGE(std::abs(math::length(world_pos(s.scene, r[a]) - world_pos(s.scene, r[b])) - lengths[a]) < 0.03f, a);
    const RagdollState rest = s.world->ragdoll_state(r.root);
    CHECK(math::length(rest.velocity) < 0.2f);
    CHECK(rest.center_of_mass.y < 0.4f);
    // The Transform of the root itself never moves: only bones show the simulation.
    CHECK(math::length(world_pos(s.scene, r.root)) < 1e-5f);
}

TEST_CASE("a kinematic ragdoll follows the animated pose, is hit by raycasts and pushes bodies") {
    register_components();
    Sim s;
    const Rig r = humanoid(s.scene);
    REQUIRE(make_humanoid_ragdoll(s.scene, r.root) == 16);
    s.scene.world().get<RagdollComponent>(r.root)->mode = Mode::Kinematic;
    // An "animator": swings the spine forward over time and walks the hips along X.
    float time = 0;
    s.sim.add_system("test.animate", gameplay::Phase::Update, [&](Scene& scene, const gameplay::FrameTime& t) {
        time += static_cast<float>(t.dt);
        scene.world().get<Transform>(r["Hips"])->position = {time * 0.5f, 1.0f, 0};
    });
    s.run(1.0f);
    const RagdollState st = s.world->ragdoll_state(r.root);
    REQUIRE(st.valid);
    CHECK(st.pose_error < 0.02f);
    // Kinematic: the scene shows the animation untouched.
    CHECK(world_pos(s.scene, r["Hips"]).x == doctest::Approx(time * 0.5f));
    // The head body sits where the animated head is.
    const math::Vec3 head = world_pos(s.scene, r["Head"]) + math::Vec3{0, 0.11f, 0}; // sphere centre
    const auto hit = s.world->raycast(head + math::Vec3{0, 0, 2}, {0, 0, -1}, 5);
    REQUIRE(hit);
    CHECK(hit->entity == r["Head"]);
    CHECK(s.world->ragdoll_of(hit->entity) == r.root);
    // Ignoring the root ignores its whole ragdoll.
    CHECK_FALSE(s.world->raycast(head + math::Vec3{0, 0, 2}, {0, 0, -1}, 5, kAllLayers, r.root));
    // Kinematic bones ignore impulses.
    s.world->add_impulse(r["Head"], {0, 0, 100});
    CHECK(math::length(s.world->linear_velocity(r["Head"]) - math::Vec3{0.5f, 0, 0}) < 0.2f);
}

TEST_CASE("a powered ragdoll with a pinned root holds the animated pose against gravity") {
    register_components();
    Sim s;
    const Rig r = humanoid(s.scene);
    REQUIRE(make_humanoid_ragdoll(s.scene, r.root) == 16);
    auto* rc = s.scene.world().get<RagdollComponent>(r.root);
    rc->mode = Mode::Powered;
    rc->pin_root = true;
    // The animator lowers the left arm by 60 degrees about Z every frame.
    const math::Quat lowered = math::Quat::axis_angle({0, 0, 1}, -1.047f);
    s.sim.add_system("test.animate", gameplay::Phase::Update, [&](Scene& scene, const gameplay::FrameTime&) {
        scene.world().get<Transform>(r["UpperArm.L"])->rotation = lowered;
    });
    s.run(2.0f);
    const RagdollState st = s.world->ragdoll_state(r.root);
    REQUIRE(st.valid);
    CHECK(st.pose_error < 0.01f);
    // The simulated hand sits where the animation puts it (shoulder + rotated arm).
    const math::Vec3 shoulder = world_pos(s.scene, r["UpperArm.L"]);
    const math::Vec3 expected = shoulder + lowered.rotate({0.53f, 0, 0});
    CHECK(math::length(world_pos(s.scene, r["Hand.L"]) - expected) < 0.02f);
    // Soft (1 Hz) motors sag visibly without gravity compensation and hold the pose with it.
    rc = s.scene.world().get<RagdollComponent>(r.root);
    rc->motor_frequency = 1.0f;
    rc->gravity_compensation = 0;
    s.run(3.0f);
    CHECK(math::length(world_pos(s.scene, r["Hand.L"]) - expected) > 0.08f);
    s.scene.world().get<RagdollComponent>(r.root)->gravity_compensation = 1;
    s.run(4.0f);
    CHECK(math::length(world_pos(s.scene, r["Hand.L"]) - expected) < 0.02f);
    // Without motors (strength 0) the arm falls limp below the shoulder.
    for (const char* n : {"UpperArm.L", "LowerArm.L", "Hand.L"}) s.scene.world().get<RagdollBoneComponent>(r[n])->strength = 0;
    s.run(2.0f);
    CHECK(world_pos(s.scene, r["Hand.L"]).y < shoulder.y - 0.3f);
}

TEST_CASE("blend 0 shows the animation while the simulation runs underneath") {
    register_components();
    Sim s;
    ground(s.scene);
    const Rig r = humanoid(s.scene);
    REQUIRE(make_humanoid_ragdoll(s.scene, r.root) == 16);
    auto* rc = s.scene.world().get<RagdollComponent>(r.root);
    rc->mode = Mode::Passive;
    rc->blend = 0;
    s.run(2.0f);
    CHECK(world_pos(s.scene, r["Hips"]).y == doctest::Approx(1.0f).epsilon(1e-3));
    CHECK(s.world->ragdoll_state(r.root).center_of_mass.y < 0.5f); // the bodies fell
    s.scene.world().get<RagdollComponent>(r.root)->blend = 1;
    s.run(kDt);
    CHECK(world_pos(s.scene, r["Hips"]).y < 0.5f);
}

TEST_CASE("switching from kinematic to passive keeps the pose and momentum without a snap") {
    register_components();
    Sim s;
    ground(s.scene);
    const Rig r = humanoid(s.scene);
    REQUIRE(make_humanoid_ragdoll(s.scene, r.root) == 16);
    s.scene.world().get<RagdollComponent>(r.root)->mode = Mode::Kinematic;
    float time = 0;
    bool animate = true;
    s.sim.add_system("test.animate", gameplay::Phase::Update, [&](Scene& scene, const gameplay::FrameTime& t) {
        if (!animate) return;
        time += static_cast<float>(t.dt);
        scene.world().get<Transform>(r["Hips"])->position = {time * 2.0f, 1.0f, 0};
    });
    s.run(0.5f);
    const math::Vec3 before = world_pos(s.scene, r["Hips"]);
    animate = false;
    s.world->set_ragdoll_mode(s.scene, r.root, Mode::Passive);
    s.run(kDt);
    const math::Vec3 after = world_pos(s.scene, r["Hips"]);
    CHECK(math::length(after - before) < 0.06f); // one step of 2 m/s motion, no teleport
    CHECK(s.world->ragdoll_state(r.root).velocity.x > 1.0f); // carried the walking speed
    s.run(1.0f);
    CHECK(world_pos(s.scene, r["Hips"]).x > before.x + 0.3f);
}

TEST_CASE("a character controller on the root ignores its own ragdoll") {
    register_components();
    Sim s;
    ground(s.scene);
    const Rig r = humanoid(s.scene);
    s.scene.world().add<CharacterControllerComponent>(r.root, CharacterControllerComponent{});
    REQUIRE(make_humanoid_ragdoll(s.scene, r.root) == 16);
    s.scene.world().get<RagdollComponent>(r.root)->mode = Mode::Kinematic;
    s.run(0.5f);
    CHECK(s.world->character_state(r.root).grounded);
    s.world->move_character(r.root, {2, 0, 0});
    s.run(1.0f);
    const math::Vec3 p = world_pos(s.scene, r.root);
    CHECK(p.x > 1.5f);
    CHECK(std::abs(p.y) < 0.1f);
    CHECK(s.world->ragdoll_state(r.root).pose_error < 0.05f);
}

TEST_CASE("ragdoll bodies are removed when the mode is off or the entity is destroyed") {
    register_components();
    Sim s;
    const Rig r = humanoid(s.scene);
    REQUIRE(make_humanoid_ragdoll(s.scene, r.root) == 16);
    s.run(kDt);
    const std::uint32_t base = s.world->stats().bodies_3d;
    CHECK_FALSE(s.world->ragdoll_state(r.root).valid);
    s.world->set_ragdoll_mode(s.scene, r.root, Mode::Passive);
    s.run(kDt);
    CHECK(s.world->stats().bodies_3d == base + 16);
    s.world->set_ragdoll_mode(s.scene, r.root, Mode::Off);
    s.run(kDt);
    CHECK(s.world->stats().bodies_3d == base);
    CHECK_FALSE(s.world->has_body(r["Hips"]));
    s.world->set_ragdoll_mode(s.scene, r.root, Mode::Passive);
    s.run(kDt);
    CHECK(s.world->stats().bodies_3d == base + 16);
    // Editing a bone rebuilds the ragdoll in place.
    s.scene.world().get<RagdollBoneComponent>(r["Head"])->radius = 0.15f;
    s.run(kDt);
    CHECK(s.world->stats().bodies_3d == base + 16);
    s.scene.destroy(r.root);
    s.run(kDt);
    CHECK(s.world->stats().bodies_3d == base);
    s.world->clear();
}

TEST_CASE("impulses on a bone push only that part of a passive ragdoll") {
    register_components();
    Sim s;
    ground(s.scene);
    const Rig r = humanoid(s.scene);
    REQUIRE(make_humanoid_ragdoll(s.scene, r.root) == 16);
    s.scene.world().get<RagdollComponent>(r.root)->mode = Mode::Passive;
    s.run(kDt);
    s.world->add_impulse(r["Head"], {0, 0, 20});
    CHECK(s.world->linear_velocity(r["Head"]).z > 1.0f);
    s.run(0.5f);
    CHECK(world_pos(s.scene, r["Head"]).z > 0.2f);
    CHECK(s.world->ragdoll_state(r.root).velocity.z > 0.0f);
}

TEST_CASE("collider_lines draws ragdoll bones") {
    register_components();
    Scene scene;
    const Rig r = humanoid(scene);
    REQUIRE(make_humanoid_ragdoll(scene, r.root) == 16);
    std::vector<DebugLine> lines;
    collider_lines(scene, lines, r["Head"]);
    CHECK(lines.size() > 30);
    lines.clear();
    collider_lines(scene, lines);
    CHECK(lines.size() > 16 * 30);
}

namespace {

float quat_dot(math::Quat a, math::Quat b) { return std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w); }

// Local bone poses of the T-posed humanoid, i.e. the animated pose get-up blends back to.
std::map<std::string, std::pair<math::Vec3, math::Quat>> local_pose(const Scene& scene, const Rig& r) {
    std::map<std::string, std::pair<math::Vec3, math::Quat>> out;
    for (const char* n : kSkeleton) {
        const auto* t = scene.world().get<Transform>(r[n]);
        out[n] = {t->position, t->rotation};
    }
    return out;
}

// Drops the humanoid as a passive ragdoll after tipping it backwards (+Z, away from its -Z front)
// or forwards, and waits until it rests.
void knock_down(Sim& s, const Rig& r, float push_z) {
    REQUIRE(make_humanoid_ragdoll(s.scene, r.root) == 16);
    s.scene.world().get<RagdollComponent>(r.root)->mode = Mode::Passive;
    s.run(kDt);
    s.world->add_impulse(r["Chest"], {0, 0, push_z});
    s.world->add_impulse(r["Head"], {0, 0, push_z * 0.5f});
    s.run(4.0f);
}

} // namespace

TEST_CASE("ragdoll get-up moves the root under the hips and blends from the lying pose to the animation") {
    register_components();
    // -10.25 and -8.5 used to recapture the lying pose as the animated target (rounding in the
    // "unchanged since we wrote it" test); keep them as regressions.
    for (const float push : {12.0f, -12.0f, -10.25f, -8.5f, 12.5f}) {
        CAPTURE(push);
        Sim s;
        ground(s.scene);
        const Rig r = humanoid(s.scene, {1, 0, 2});
        const auto animated = local_pose(s.scene, r);
        knock_down(s, r, push);
        std::map<std::string, math::Vec3> lying;
        for (const char* n : kSkeleton) lying[n] = world_pos(s.scene, r[n]);
        const math::Vec3 hips = lying["Hips"];
        const math::Vec3 head = lying["Head"];

        const RagdollGetUp g = s.world->begin_ragdoll_get_up(s.scene, r.root, 0.5f);
        REQUIRE_MESSAGE(g.started, g.reason);
        // Pushed backwards (+Z) the body lands on its back.
        CHECK(g.face_up == (push > 0));
        CHECK(std::abs(g.position.y) < 0.05f); // on the ground
        CHECK(std::hypot(g.position.x - hips.x, g.position.z - hips.z) < 0.05f);
        CHECK(math::length(world_pos(s.scene, r.root) - g.position) < 1e-4f);
        // Face up the character faces its feet (away from the head), face down it faces the head.
        const math::Vec3 facing = g.rotation.rotate({0, 0, -1});
        math::Vec3 to_head{head.x - hips.x, 0, head.z - hips.z};
        to_head = math::normalize(to_head);
        CHECK(math::dot(facing, to_head) * (g.face_up ? -1.0f : 1.0f) > 0.7f);
        CHECK(s.scene.world().get<RagdollComponent>(r.root)->mode == Mode::Kinematic);

        // First frame: no pop — the bones are where the ragdoll lay.
        s.run(kDt);
        const RagdollState st = s.world->ragdoll_state(r.root);
        CHECK(st.getting_up);
        CHECK(st.get_up_progress > 0.0f);
        CHECK(st.get_up_progress < 0.1f);
        for (const char* n : kSkeleton) CHECK_MESSAGE(math::length(world_pos(s.scene, r[n]) - lying[n]) < 0.06f, n);

        // Halfway the pose is in between; after the blend the animation owns the bones again.
        s.run(0.25f);
        CHECK(s.world->ragdoll_state(r.root).get_up_progress > 0.4f);
        s.run(0.5f);
        CHECK_FALSE(s.world->ragdoll_state(r.root).getting_up);
        for (const char* n : kSkeleton) {
            const auto* t = s.scene.world().get<Transform>(r[n]);
            CHECK_MESSAGE(math::length(t->position - animated.at(n).first) < 1e-3f, n);
            CHECK_MESSAGE(quat_dot(t->rotation, animated.at(n).second) > 0.9999f, n);
        }
        // Standing on the ground at the new place: feet near y = 0, hips 1 m up.
        CHECK(std::abs(world_pos(s.scene, r["Hips"]).y - 1.0f) < 0.05f);
        // Bodies follow the animation kinematically (raycasts hit the standing character).
        const auto hit = s.world->raycast(world_pos(s.scene, r["Chest"]) + math::Vec3{0, 0, 3}, {0, 0, -1}, 6.0f);
        REQUIRE(hit);
        CHECK(s.world->ragdoll_of(hit->entity) == r.root);
    }
}

TEST_CASE("ragdoll get-up is rejected unless the ragdoll simulates, and a new knock-down cancels it") {
    register_components();
    Sim s;
    ground(s.scene);
    const Rig r = humanoid(s.scene);
    CHECK_FALSE(s.world->begin_ragdoll_get_up(s.scene, r.root).started); // no ragdoll
    REQUIRE(make_humanoid_ragdoll(s.scene, r.root) == 16);
    s.scene.world().get<RagdollComponent>(r.root)->mode = Mode::Kinematic;
    s.run(kDt);
    const RagdollGetUp k = s.world->begin_ragdoll_get_up(s.scene, r.root);
    CHECK_FALSE(k.started);
    CHECK_FALSE(k.reason.empty());

    s.scene.world().get<RagdollComponent>(r.root)->mode = Mode::Passive;
    s.run(2.0f);
    REQUIRE(s.world->begin_ragdoll_get_up(s.scene, r.root, 1.0f).started);
    s.run(0.2f);
    CHECK(s.world->ragdoll_state(r.root).getting_up);
    s.scene.world().get<RagdollComponent>(r.root)->mode = Mode::Passive; // hit again mid get-up
    s.run(kDt);
    CHECK_FALSE(s.world->ragdoll_state(r.root).getting_up);
    s.run(1.0f);
    for (const char* n : kSkeleton) CHECK_MESSAGE(finite(world_pos(s.scene, r[n])), n);
}
