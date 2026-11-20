// SPDX-License-Identifier: Apache-2.0
// The Orion and Nova starter characters (docs/CHARACTERS.md): both GLBs import through the asset
// database without warnings, share one skeleton and clip library, meet the size and budget
// targets, match the template scene's hierarchy, and the shared controller drives them.
#include "doctest.h"
#include "victus/animation/animation.hpp"
#include "victus/assets/database.hpp"
#include "victus/assets/importers.hpp"
#include "victus/assets/instantiate.hpp"
#include "victus/core/fs.hpp"
#include "victus/core/json.hpp"
#include "victus/project/characters.hpp"
#include "victus/project/project.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <filesystem>
#include <set>

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

std::string model_rel(const project::StarterCharacter& c) {
    return std::string(c.model_path.substr(std::string_view("assets/").size()));
}

// Position of `e` in the space of `root` (the character's model space).
math::Vec3 model_space(scene::Scene& s, scene::Entity root, scene::Entity e) {
    s.update_transforms();
    return math::inverse(s.world_matrix(root)).transform_point(s.world_matrix(e).transform_point({0, 0, 0}));
}

} // namespace

TEST_CASE("starter characters: Orion and Nova import cleanly with one shared skeleton and clip library") {
    TempDir dir("victus_tpl_chars");
    project::register_components();
    auto p = project::Project::create(dir.path, "Mannequins", "3d");
    REQUIRE(p.ok());
    auto db = assets::AssetDatabase::open(dir.path);
    REQUIRE(db.ok());
    REQUIRE(project::starter_characters().size() == 2);

    std::vector<assets::CookedModel> models;
    for (const auto& c : project::starter_characters()) {
        INFO(c.id);
        const assets::AssetRecord* rec = (*db)->find_path(model_rel(c));
        REQUIRE(rec);
        CHECK(rec->uuid.to_string() == c.uuid); // stable identity from the .meta
        auto meta = fs::read_text(dir.path / (std::string(c.model_path) + ".meta"), 1u << 16);
        REQUIRE(meta.ok());
        // The written GLB is the embedded one.
        auto bytes = fs::read_binary(dir.path / std::string(c.model_path), 1u << 26);
        REQUIRE(bytes.ok());
        const auto glb = project::starter_character_glb(c);
        CHECK(bytes->size() == glb.size());
        CHECK(std::equal(glb.begin(), glb.end(), bytes->begin()));

        std::vector<std::string> messages;
        REQUIRE((*db)->import(rec->uuid, &messages).ok());
        for (const auto& m : messages) MESSAGE(m);
        CHECK(messages.empty()); // zero importer warnings
        auto model = (*db)->load_model(rec->uuid);
        REQUIRE(model.ok());
        CHECK(model->meshes.size() == 4); // shell, structure, visor, glow
        CHECK(model->materials.size() == 4);
        REQUIRE(model->skins.size() == 1);
        CHECK(model->skins[0].joints.size() == project::kCharacterJointCount);
        // Budget: 20k-35k triangles at the highest LOD, with generated coarser LODs.
        const std::size_t tris = model->triangle_count();
        MESSAGE(c.id << ": " << tris << " triangles");
        CHECK(tris >= 20000);
        CHECK(tris <= 35000);
        for (const auto& m : model->meshes) {
            CHECK(m.data.skinned());
            CHECK(m.data.lods.size() >= 1);
        }
        // 1.70 m tall, feet on the ground plane, arms in an A-pose.
        math::Aabb bounds;
        for (const auto& m : model->meshes)
            for (const auto& v : m.data.vertices) bounds.expand(v.position);
        CHECK(bounds.min.y == doctest::Approx(0.0f).epsilon(0.005));
        // 1.70 m to the crown of the sculpted hair (within a couple of centimetres).
        CHECK(bounds.max.y >= project::kCharacterHeight - 0.01f);
        CHECK(bounds.max.y <= project::kCharacterHeight + 0.03f);
        CHECK(bounds.max.x == doctest::Approx(-bounds.min.x).epsilon(0.01)); // symmetric
        // Clips (seconds) and sockets.
        const std::pair<const char*, float> clips[] = {{"Idle", 4.0f}, {"Walk", 0.8f}, {"Run", 0.55f}, {"Jump", 0.6f},
                                                       {"Fall", 0.8f}, {"Land", 0.4f}, {"Interact", 0.9f}};
        REQUIRE(model->animations.size() == std::size(clips));
        for (const auto& [name, seconds] : clips) {
            const auto it = std::find_if(model->animations.begin(), model->animations.end(),
                                         [&](const animation::Clip& k) { return k.name == name; });
            INFO(name);
            REQUIRE(it != model->animations.end());
            CHECK(it->duration == doctest::Approx(seconds).epsilon(0.01));
        }
        const std::pair<const char*, const char*> sockets[] = {{"Socket.Head", "Head"},     {"Socket.Hand.L", "Hand.L"},
                                                               {"Socket.Hand.R", "Hand.R"}, {"Socket.Back", "Chest"},
                                                               {"Socket.Root", "Rig"}};
        for (const auto& [socket, parent] : sockets) {
            const auto it = std::find_if(model->nodes.begin(), model->nodes.end(),
                                         [&](const assets::ModelNode& n) { return n.name == socket; });
            INFO(socket);
            REQUIRE(it != model->nodes.end());
            REQUIRE(it->parent >= 0);
            CHECK(model->nodes[static_cast<std::size_t>(it->parent)].name == parent);
        }
        models.push_back(std::move(model).value());
    }

    // Same joint names, order and bind pose in both variants: clips and scripts are shared.
    const auto& a = models[0];
    const auto& b = models[1];
    REQUIRE(a.skins[0].joints.size() == b.skins[0].joints.size());
    for (std::size_t j = 0; j < a.skins[0].joints.size(); ++j) {
        const auto& na = a.nodes[a.skins[0].joints[j]];
        const auto& nb = b.nodes[b.skins[0].joints[j]];
        CHECK(na.name == nb.name);
        CHECK(math::length(na.translation - nb.translation) < 1e-5f);
        for (int col = 0; col < 4; ++col) {
            const math::Vec4 x = a.skins[0].inverse_bind[j].c[col];
            const math::Vec4 y = b.skins[0].inverse_bind[j].c[col];
            CHECK(std::abs(x.x - y.x) + std::abs(x.y - y.y) + std::abs(x.z - y.z) + std::abs(x.w - y.w) < 1e-4f);
        }
    }
    const std::set<std::string> core = {"Hips", "Spine", "Head", "UpperArm.L", "LowerArm.L", "UpperArm.R", "LowerArm.R",
                                        "UpperLeg.L", "LowerLeg.L", "UpperLeg.R", "LowerLeg.R"};
    std::set<std::string> joints;
    for (const auto j : a.skins[0].joints) joints.insert(a.nodes[j].name);
    for (const auto& n : core) CHECK(joints.count(n) == 1); // the template character's joint names survive
    // The two variants are distinct designs, not recolours.
    CHECK(a.triangle_count() != b.triangle_count());
}

TEST_CASE("starter characters: template scene mirrors the model and the shared controller drives it") {
    TempDir dir("victus_tpl_chars_scene");
    project::register_components();
    auto p = project::Project::create(dir.path, "Mannequins", "3d");
    REQUIRE(p.ok());
    auto db = assets::AssetDatabase::open(dir.path);
    REQUIRE(db.ok());
    const auto* v01 = project::find_starter_character("orion");
    REQUIRE(v01);
    const assets::AssetRecord* rec = (*db)->find_path(model_rel(*v01));
    REQUIRE(rec);
    REQUIRE((*db)->import(rec->uuid).ok());
    auto model = (*db)->load_model(rec->uuid);
    REQUIRE(model.ok());

    // Player > Robot is exactly what instantiate_model creates for the imported model.
    scene::Scene s;
    auto scene_path = p->startup_scene_path();
    REQUIRE(scene_path.ok());
    REQUIRE(s.load_file(*scene_path).ok());
    const scene::Entity robot = s.find_by_name("Robot");
    REQUIRE_FALSE(robot.is_null());
    CHECK(s.parent(robot) == s.find_by_name("Player"));
    scene::Scene ref;
    const scene::Entity ref_root = assets::instantiate_model(ref, rec->uuid, *model, "Robot");
    std::function<void(scene::Entity, scene::Entity)> same = [&](scene::Entity x, scene::Entity y) {
        const auto cx = s.children(x);
        const auto cy = ref.children(y);
        REQUIRE(cx.size() == cy.size());
        for (std::size_t i = 0; i < cx.size(); ++i) {
            CHECK(s.world().get<scene::Name>(cx[i])->value == ref.world().get<scene::Name>(cy[i])->value);
            CHECK(math::length(s.world().get<scene::Transform>(cx[i])->position -
                               ref.world().get<scene::Transform>(cy[i])->position) < 1e-6f);
            const auto* mx = s.world().get<scene::MeshRendererComponent>(cx[i]);
            const auto* my = ref.world().get<scene::MeshRendererComponent>(cy[i]);
            CHECK((mx == nullptr) == (my == nullptr));
            if (mx && my) CHECK(mx->mesh_index == my->mesh_index);
            same(cx[i], cy[i]);
        }
    };
    same(robot, ref_root);
    const auto* animator = s.world().get<animation::AnimatorComponent>(robot);
    REQUIRE(animator);
    CHECK(animator->model == rec->uuid);
    CHECK(animator->controller == project::kCharacterControllerAsset);
    // The companion is the other character (Nova) at companion size.
    const scene::Entity buddy = s.find_by_name("Buddy");
    REQUIRE_FALSE(buddy.is_null());
    scene::Entity buddy_model;
    for (const auto c : s.children(buddy))
        if (s.world().get<scene::Name>(c)->value == "Robot") buddy_model = c;
    REQUIRE_FALSE(buddy_model.is_null());
    CHECK(s.world().get<animation::AnimatorComponent>(buddy_model)->model.to_string() ==
          project::find_starter_character("nova")->uuid);

    // The project's loader runs the shared controller: Locomotion by speed, Jump -> Fall while
    // airborne -> Land on touch-down -> Locomotion, and Interact from its trigger.
    animation::AnimationSettings cs;
    cs.provider = assets::animation_provider(**db);
    cs.controllers = animation::directory_loader(dir.path / "assets");
    animation::AnimationSystem ctl(cs);
    ctl.update(s, 0.0f);
    auto st = ctl.controller_status(s, robot);
    REQUIRE(st.valid);
    CHECK(st.error.empty());
    CHECK(st.state == "Locomotion");
    REQUIRE(st.weights.size() == 3);
    for (const auto& w : st.weights) CHECK_FALSE(w.missing);
    CHECK(ctl.status(s, robot).bound_nodes >= project::kCharacterJointCount);

    // Idle keeps the feet planted: the ankles stay at their rest height (0.09 m) in model space.
    const scene::Entity foot = s.find_by_name("Foot.L");
    const scene::Entity knee = s.find_by_name("LowerLeg.L");
    const scene::Entity hips = s.find_by_name("Hips");
    for (int i = 0; i < 4; ++i) {
        ctl.update(s, 0.5f);
        CHECK(model_space(s, robot, foot).y == doctest::Approx(0.09f).epsilon(0.1));
    }
    // Walk strides: the legs swing in opposition, about 0.2 m each side of the hips (the clip
    // starts with the left leg back; the model faces +Z).
    REQUIRE(ctl.set_parameter(s, robot, "speed", 2.5f));
    ctl.update(s, 0.0f);
    const float hz = model_space(s, robot, hips).z;
    CHECK(model_space(s, robot, knee).z < hz - 0.1f);
    CHECK(model_space(s, robot, s.find_by_name("LowerLeg.R")).z > hz + 0.1f);

    REQUIRE(ctl.set_trigger(s, robot, "jump"));
    ctl.update(s, 1.0f / 60);
    CHECK(ctl.controller_status(s, robot).state == "Jump");
    REQUIRE(ctl.set_parameter(s, robot, "grounded", 0.0f));
    for (int i = 0; i < 50; ++i) ctl.update(s, 1.0f / 60);
    CHECK(ctl.controller_status(s, robot).state == "Fall");
    REQUIRE(ctl.set_parameter(s, robot, "grounded", 1.0f));
    ctl.update(s, 1.0f / 60);
    CHECK(ctl.controller_status(s, robot).state == "Land");
    for (int i = 0; i < 40; ++i) ctl.update(s, 1.0f / 60);
    CHECK(ctl.controller_status(s, robot).state == "Locomotion");
    REQUIRE(ctl.set_trigger(s, robot, "interact"));
    ctl.update(s, 1.0f / 60);
    CHECK(ctl.controller_status(s, robot).state == "Interact");
    for (int i = 0; i < 70; ++i) ctl.update(s, 1.0f / 60);
    CHECK(ctl.controller_status(s, robot).state == "Locomotion");
}

TEST_CASE("starter characters: template options pick Nova or no character") {
    project::register_components();
    {
        TempDir dir("victus_tpl_nova");
        project::TemplateOptions o;
        o.character = "nova";
        auto p = project::Project::create(dir.path, "NovaGame", "3d", o);
        REQUIRE(p.ok());
        scene::Scene s;
        REQUIRE(s.load_file(*p->startup_scene_path()).ok());
        const scene::Entity robot = s.find_by_name("Robot");
        REQUIRE_FALSE(robot.is_null());
        CHECK(s.world().get<animation::AnimatorComponent>(robot)->model.to_string() ==
              project::find_starter_character("nova")->uuid);
        CHECK_FALSE(s.find_by_name("Nova_Body[0]").is_null());
        CHECK(fs::exists(dir.path / "assets/models/characters/victus_orion.glb")); // swap target ships too
    }
    {
        TempDir dir("victus_tpl_nochar");
        project::TemplateOptions o;
        o.character = std::string(project::kNoCharacter);
        auto p = project::Project::create(dir.path, "Plain", "3d", o);
        REQUIRE(p.ok());
        scene::Scene s;
        REQUIRE(s.load_file(*p->startup_scene_path()).ok());
        CHECK(s.find_by_name("Robot").is_null());
        const scene::Entity body = s.find_by_name("Body");
        REQUIRE_FALSE(body.is_null());
        CHECK(s.parent(body) == s.find_by_name("Player"));
        CHECK(s.world().has<scene::MeshRendererComponent>(body));
        CHECK_FALSE(fs::exists(dir.path / "assets/models/characters"));
        CHECK_FALSE(fs::exists(dir.path / project::kCharacterControllerPath));
    }
    {
        TempDir dir("victus_tpl_badchar");
        project::TemplateOptions o;
        o.character = "v03";
        auto p = project::Project::create(dir.path, "Bad", "3d", o);
        REQUIRE_FALSE(p.ok());
        CHECK(p.error().message.find("v03") != std::string::npos);
    }
    {
        // The multiplayer arena's player template uses the same character; 2D does not get one.
        TempDir mp("victus_tpl_mp");
        auto p = project::Project::create(mp.path, "Arena", "multiplayer");
        REQUIRE(p.ok());
        scene::Scene s;
        REQUIRE(s.load_file(*p->startup_scene_path()).ok());
        CHECK_FALSE(s.find_by_name("Orion_Body[0]").is_null());
        TempDir d2("victus_tpl_2d");
        REQUIRE(project::Project::create(d2.path, "Flat", "2d").ok());
        CHECK_FALSE(fs::exists(d2.path / "assets/models/characters"));
    }
}

TEST_CASE("assets: import_gltf_memory reads self-contained models and rejects external references") {
    const auto* c = project::find_starter_character("orion");
    REQUIRE(c);
    assets::ImportResult res;
    auto m = assets::import_gltf_memory(project::starter_character_glb(*c), "orion.glb", {}, &res);
    REQUIRE(m.ok());
    CHECK(res.warnings.empty());
    CHECK(m->skins.size() == 1);
    const std::string external = R"({"asset":{"version":"2.0"},"buffers":[{"uri":"data.bin","byteLength":4}]})";
    auto bad = assets::import_gltf_memory({reinterpret_cast<const std::uint8_t*>(external.data()), external.size()},
                                          "ext.gltf", {});
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().message.find("external reference") != std::string::npos);
}
