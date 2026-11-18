// SPDX-License-Identifier: Apache-2.0
// The 3D template's procedural robot: its glTF imports cleanly with a skin and Idle/Walk clips,
// the template scene's hierarchy matches the cooked model, and the clips drive the joints.
#include "doctest.h"
#include "victus/animation/animation.hpp"
#include "victus/assets/database.hpp"
#include "victus/core/fs.hpp"
#include "victus/core/json.hpp"
#include "victus/project/project.hpp"

#include <cmath>
#include <filesystem>
#include <numbers>

using namespace victus;

namespace {

struct TempDir {
    fs::Path path;
    explicit TempDir(const char* name) {
        path = std::filesystem::temp_directory_path() / (std::string(name) + "_" + Uuid::generate().to_string().substr(0, 8));
        std::filesystem::remove_all(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

bool same_rotation(math::Quat a, math::Quat b, float eps = 1e-3f) {
    return std::abs(std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) - 1.0f) <= eps;
}

math::Quat rx(float deg) { return math::Quat::axis_angle({1, 0, 0}, deg * std::numbers::pi_v<float> / 180.0f); }

} // namespace

TEST_CASE("template robot: imports with a skin and clips, matches the scene and animates") {
    TempDir dir("victus_tpl_robot");
    project::register_components();
    auto p = project::Project::create(dir.path, "Robot", "3d");
    REQUIRE(p.ok());
    auto db = assets::AssetDatabase::open(dir.path);
    REQUIRE(db.ok());
    const assets::AssetRecord* rec = (*db)->find_path("models/robot.gltf");
    REQUIRE(rec);
    // The template ships a .meta with a fixed identity the scene refers to.
    auto meta = fs::read_text(dir.path / "assets" / "models" / "robot.gltf.meta", 1u << 16);
    REQUIRE(meta.ok());
    auto meta_doc = json::parse(*meta);
    REQUIRE(meta_doc.ok());
    CHECK((*meta_doc)["uuid"].as_string() == rec->uuid.to_string());

    std::vector<std::string> messages;
    REQUIRE((*db)->import(rec->uuid, &messages).ok());
    for (const auto& m : messages) MESSAGE(m);
    CHECK(messages.empty()); // no importer warnings
    auto model = (*db)->load_model(rec->uuid);
    REQUIRE(model.ok());
    REQUIRE(model->meshes.size() == 2);
    REQUIRE(model->skins.size() == 1);
    CHECK(model->skins[0].joints.size() == 11);
    REQUIRE(model->animations.size() == 4);
    CHECK(model->animations[0].name == "Idle");
    CHECK(model->animations[0].duration == doctest::Approx(2.4f));
    CHECK(model->animations[1].name == "Walk");
    CHECK(model->animations[1].duration == doctest::Approx(0.8f));
    CHECK(model->animations[2].name == "Run");
    CHECK(model->animations[2].duration == doctest::Approx(0.55f));
    CHECK(model->animations[3].name == "Jump");
    CHECK(model->animations[3].duration == doctest::Approx(0.6f));
    math::Aabb bounds;
    for (const auto& m : model->meshes) {
        CHECK(m.data.skinned());
        for (const auto& v : m.data.vertices) bounds.expand(v.position);
    }
    CHECK(bounds.min.y == doctest::Approx(0.0f));
    CHECK(bounds.max.y == doctest::Approx(1.67f));
    CHECK(model->materials.size() == 2);

    // Template scene: Player > Robot mirrors the model nodes (names, parents, rest translations).
    scene::Scene s;
    auto scene_path = p->startup_scene_path();
    REQUIRE(scene_path.ok());
    REQUIRE(s.load_file(*scene_path).ok());
    const scene::Entity robot = s.find_by_name("Robot");
    REQUIRE_FALSE(robot.is_null());
    CHECK(s.parent(robot) == s.find_by_name("Player"));
    std::vector<scene::Entity> nodes(model->nodes.size());
    for (std::size_t i = 0; i < model->nodes.size(); ++i) {
        const auto& n = model->nodes[i];
        const scene::Entity parent = n.parent < 0 ? robot : nodes[static_cast<std::size_t>(n.parent)];
        for (const scene::Entity c : s.children(parent))
            if (s.world().get<scene::Name>(c)->value == n.name) nodes[i] = c;
        INFO(n.name);
        REQUIRE_FALSE(nodes[i].is_null());
        const auto* t = s.world().get<scene::Transform>(nodes[i]);
        CHECK(math::length(t->position - n.translation) < 1e-5f);
        CHECK(same_rotation(t->rotation, n.rotation, 1e-6f));
        if (n.meshes.size() > 1) {
            for (const std::uint32_t mi : n.meshes) {
                const scene::Entity part = s.find_by_name(n.name + "[" + std::to_string(mi) + "]");
                REQUIRE_FALSE(part.is_null());
                const auto* mr = s.world().get<scene::MeshRendererComponent>(part);
                REQUIRE(mr);
                CHECK(mr->mesh_asset == rec->uuid);
                CHECK(mr->mesh_index == mi);
                const auto* sk = s.world().get<animation::SkinnedMeshComponent>(part);
                REQUIRE(sk);
                CHECK(sk->skeleton == s.uuid(robot));
                CHECK(sk->skin == static_cast<std::uint32_t>(n.skin));
            }
        }
    }

    // The database provider feeds the animation system: Idle binds, Walk swings the legs.
    animation::AnimationSettings settings;
    settings.provider = assets::animation_provider(**db);
    animation::AnimationSystem anim(settings);
    anim.update(s, 0.0f);
    auto st = anim.status(s, robot);
    CHECK(st.valid);
    CHECK(st.clip == "Idle");
    CHECK(st.bound_nodes == 12);
    REQUIRE(anim.play(s, robot, "Walk", 0.0f));
    anim.update(s, 0.0f);
    const scene::Entity leg = s.find_by_name("UpperLeg.L");
    const scene::Entity other = s.find_by_name("UpperLeg.R");
    CHECK(same_rotation(s.world().get<scene::Transform>(leg)->rotation, rx(-28.0f)));  // left leg forward
    CHECK(same_rotation(s.world().get<scene::Transform>(other)->rotation, rx(28.0f))); // right leg back
    anim.update(s, 0.4f); // half a stride
    CHECK(same_rotation(s.world().get<scene::Transform>(leg)->rotation, rx(28.0f)));
    CHECK(anim.status(s, robot).looping);

    // With the project's controller loader the template's anim/robot.vanim drives the robot:
    // Idle/Walk/Run blend by "speed" (phase-synced, left leg forward at phase 0) and the "jump"
    // trigger enters Air, which returns to Locomotion once grounded after its exit time.
    scene::Scene s2;
    REQUIRE(s2.load_file(*scene_path).ok());
    const scene::Entity robot2 = s2.find_by_name("Robot");
    CHECK(s2.world().get<animation::AnimatorComponent>(robot2)->controller == "anim/robot.vanim");
    animation::AnimationSettings cs;
    cs.provider = assets::animation_provider(**db);
    cs.controllers = animation::directory_loader(dir.path / "assets");
    animation::AnimationSystem ctl(cs);
    ctl.update(s2, 0.0f);
    auto cst = ctl.controller_status(s2, robot2);
    REQUIRE(cst.valid);
    CHECK(cst.error.empty());
    CHECK(cst.state == "Locomotion");
    REQUIRE(cst.weights.size() == 3);
    CHECK(cst.weights[0].weight == doctest::Approx(1.0f)); // Idle at speed 0
    for (const auto& w : cst.weights) CHECK_FALSE(w.missing);
    REQUIRE(ctl.set_parameter(s2, robot2, "speed", 5.0f));
    ctl.update(s2, 0.0f);
    const scene::Entity leg2 = s2.find_by_name("UpperLeg.L");
    CHECK(same_rotation(s2.world().get<scene::Transform>(leg2)->rotation, rx(-55.0f))); // full Run stride
    REQUIRE(ctl.set_parameter(s2, robot2, "speed", 2.5f));
    ctl.update(s2, 0.0f);
    CHECK(same_rotation(s2.world().get<scene::Transform>(leg2)->rotation, rx(-28.0f))); // pure Walk
    REQUIRE(ctl.set_trigger(s2, robot2, "jump"));
    ctl.update(s2, 1.0f / 60);
    CHECK(ctl.controller_status(s2, robot2).state == "Air");
    for (int i = 0; i < 30; ++i) ctl.update(s2, 1.0f / 60); // past the exit time, still grounded
    CHECK(ctl.controller_status(s2, robot2).state == "Locomotion");
    // Walking off a ledge (not grounded) also enters Air.
    REQUIRE(ctl.set_parameter(s2, robot2, "grounded", 0.0f));
    for (int i = 0; i < 20; ++i) ctl.update(s2, 1.0f / 60);
    CHECK(ctl.controller_status(s2, robot2).state == "Air");
}
