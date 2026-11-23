// SPDX-License-Identifier: Apache-2.0
// The Orion and Nova starter characters (docs/CHARACTERS.md, ADR-0179): both GLBs import through
// the asset database without warnings, share one 60-joint skeleton layout (the 21 core joints
// first) and the clip library, carry the facial blend shapes, PBR texture sets and sockets, meet
// the triangle budget, match the template scene's hierarchy, and the shared controller drives them.
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
#include <optional>
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
        // The Frame mannequin (ADR-0186) with both head variants (ADR-0247): Frame (undersuit
        // body), Armor (plates, collar, boots, back core), the helmet set Visor (faceplate and
        // face-display glyphs) + Helmet (shell, ear pods), and the bare-head set Skin (face and
        // neck), Eye (eyes, teeth, tongue) + Hair (scalp, brows, lashes, strand cards). One skinned
        // primitive each on the body node (docs/CHARACTERS.md#parts), in kCharacter*Mesh order.
        const char* parts[] = {"Frame", "Armor", "Visor", "Helmet", "Skin", "Eye", "Hair"};
        static_assert(std::size(parts) == project::kCharacterMeshCount);
        REQUIRE(model->meshes.size() == std::size(parts));
        REQUIRE(model->materials.size() == std::size(parts));
        for (std::size_t i = 0; i < std::size(parts); ++i) {
            const auto& mat = model->materials[i];
            INFO(mat.name);
            CHECK(mat.name == std::string(c.display_name) + "_" + parts[i]);
            CHECK(model->meshes[i].material == static_cast<std::uint32_t>(i));
            // Real texture sets, not flat colours: albedo, tangent-space normal, ORM and the
            // emissive light trims on every part.
            CHECK(mat.base_color_texture >= 0);
            CHECK(mat.normal_texture >= 0);
            CHECK(mat.metallic_roughness_texture >= 0);
            // Emissive light trims on the suit, armour, visor and helmet; the bare head has none.
            if (i < project::kCharacterSkinMesh) CHECK(mat.emissive_texture >= 0);
            if (i == project::kCharacterHairMesh) {
                // Strand cards: alpha-tested, both faces; Nova's bob has an anisotropic highlight
                // along the strands (Orion's close crop is isotropic).
                CHECK(mat.alpha_cutoff == doctest::Approx(0.4f));
                CHECK(mat.double_sided);
                CHECK(mat.anisotropy >= 0.0f);
                if (std::string(c.id) == "nova") CHECK(mat.anisotropy > 0.0f);
            } else {
                CHECK(mat.alpha_cutoff == 0.0f); // fully opaque
            }
        }
        // Skin scatters light (KHR_materials_diffuse_transmission -> subsurface).
        CHECK(model->materials[project::kCharacterSkinMesh].subsurface_color[0] > 0.0f);
        REQUIRE(model->skins.size() == 1);
        CHECK(model->skins[0].joints.size() == project::kCharacterJointCount);
        // High-end budget at LOD0 (with generated coarser LODs): 140k-260k triangles drawn for
        // either head variant; the file carries both heads.
        const std::size_t tris = model->triangle_count();
        std::size_t helmet_tris = 0, head_tris = 0;
        for (std::size_t i = 0; i < model->meshes.size(); ++i) {
            const std::size_t t = model->meshes[i].data.indices.size() / 3;
            if (project::character_mesh_is_helmet(static_cast<std::uint32_t>(i))) helmet_tris += t;
            if (project::character_mesh_is_head(static_cast<std::uint32_t>(i))) head_tris += t;
        }
        MESSAGE(c.id << ": " << tris << " triangles (helmet " << helmet_tris << ", bare head " << head_tris << ")");
        CHECK(tris - head_tris >= 140000);
        CHECK(tris - head_tris <= 260000);
        CHECK(tris - helmet_tris >= 140000);
        CHECK(tris - helmet_tris <= 260000);
        for (std::size_t i = 0; i < model->meshes.size(); ++i) {
            const auto& m = model->meshes[i];
            CHECK(m.data.skinned());
            CHECK(m.data.lods.size() >= 1);
            // Every primitive carries the same 26 face-display blend shapes (jawOpen,
            // eyeBlink_L ...); the Visor's glyphs, the face, eyes and lashes move.
            CHECK(m.data.morphs.size() == 26);
        }
        // Feet on the ground plane, arms in an A-pose, the character's own height to the top of the
        // helmet (the taller head variant).
        math::Aabb bounds;
        for (const auto& m : model->meshes)
            for (const auto& v : m.data.vertices) bounds.expand(v.position);
        CHECK(bounds.min.y == doctest::Approx(0.0f).epsilon(0.005));
        MESSAGE(c.id << ": " << bounds.max.y << " m tall");
        CHECK(bounds.max.y == doctest::Approx(c.height).epsilon(0.01));
        CHECK(bounds.max.x == doctest::Approx(-bounds.min.x).epsilon(0.01)); // symmetric
        // Clips (seconds): the original seven keep their durations; the library adds strafes,
        // crouch, turns, combat, emotes, gestures and facial clips (docs/ANIMATION.md).
        const std::pair<const char*, float> clips[] = {{"Idle", 4.0f}, {"Walk", 0.8f}, {"Run", 0.55f}, {"Jump", 0.6f},
                                                       {"Fall", 0.8f}, {"Land", 0.4f}, {"Interact", 0.9f}};
        CHECK(model->animations.size() >= 40);
        for (const auto& [name, seconds] : clips) {
            const auto it = std::find_if(model->animations.begin(), model->animations.end(),
                                         [&](const animation::Clip& k) { return k.name == name; });
            INFO(name);
            REQUIRE(it != model->animations.end());
            CHECK(it->duration == doctest::Approx(seconds).epsilon(0.01));
        }
        const std::pair<const char*, const char*> sockets[] = {{"Socket.Head", "Head"},     {"Socket.Hand.L", "Hand.L"},
                                                               {"Socket.Hand.R", "Hand.R"}, {"Socket.Back", "Chest"},
                                                               {"Socket.Root", "Rig"},      {"Socket.Face", "Head"},
                                                               {"Socket.Hip.L", "Hips"},    {"Socket.Hip.R", "Hips"}};
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

    // Same joint names, order and hierarchy in both variants: clips and scripts are shared. The
    // bind poses differ (each character has its own proportions - skeleton profiles
    // VictusHumanoid.Orion / VictusHumanoid.Nova); clips store rotations plus the Hips translation.
    const auto& a = models[0];
    const auto& b = models[1];
    REQUIRE(a.skins[0].joints.size() == b.skins[0].joints.size());
    auto parent_name = [](const assets::CookedModel& m, std::uint32_t node) {
        const int up = m.nodes[node].parent;
        return up < 0 ? std::string() : m.nodes[static_cast<std::size_t>(up)].name;
    };
    for (std::size_t j = 0; j < a.skins[0].joints.size(); ++j) {
        CHECK(a.nodes[a.skins[0].joints[j]].name == b.nodes[b.skins[0].joints[j]].name);
        CHECK(parent_name(a, a.skins[0].joints[j]) == parent_name(b, b.skins[0].joints[j]));
    }
    // The original 21-joint layout comes first, in its original order.
    const char* original[] = {"Hips",       "Spine",      "Chest",      "Neck",       "Head",       "Shoulder.L", "UpperArm.L",
                              "LowerArm.L", "Hand.L",     "Shoulder.R", "UpperArm.R", "LowerArm.R", "Hand.R",     "UpperLeg.L",
                              "LowerLeg.L", "Foot.L",     "Toe.L",     "UpperLeg.R", "LowerLeg.R", "Foot.R",     "Toe.R"};
    static_assert(std::size(original) == project::kCharacterCoreJointCount);
    for (std::size_t j = 0; j < std::size(original); ++j) CHECK(a.nodes[a.skins[0].joints[j]].name == original[j]);
    std::set<std::string> extra;
    for (const auto j : a.skins[0].joints) extra.insert(a.nodes[j].name);
    for (const char* n : {"Thumb1.L", "Index3.L", "Pinky3.R", "UpperArmTwist.L", "LowerArmTwist.R", "UpperLegTwist.L", "Jaw",
                          "Eye.L", "Eye.R"})
        CHECK(extra.count(n) == 1);
    // Orion is the taller of the two.
    CHECK(project::find_starter_character("orion")->height > project::find_starter_character("nova")->height);
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

    // Idle keeps the feet planted: the ankles stay at their rest height in model space.
    const scene::Entity foot = s.find_by_name("Foot.L");
    const scene::Entity knee = s.find_by_name("LowerLeg.L");
    const scene::Entity hips = s.find_by_name("Hips");
    const float rest_ankle = model_space(s, robot, foot).y;
    CHECK(rest_ankle > 0.05f);
    CHECK(rest_ankle < 0.11f);
    // ...and lets the arms hang from the rest A-pose: shoulder to wrist within about 25 degrees of
    // vertical (the bind pose holds them about 45 degrees out, which an idle must not keep).
    const scene::Entity shoulder = s.find_by_name("UpperArm.L");
    const scene::Entity wrist = s.find_by_name("Hand.L");
    for (int i = 0; i < 4; ++i) {
        ctl.update(s, 0.5f);
        CHECK(model_space(s, robot, foot).y == doctest::Approx(rest_ankle).epsilon(0.1));
        const math::Vec3 arm = model_space(s, robot, shoulder) - model_space(s, robot, wrist);
        INFO("idle arm drop " << arm.y / math::length(arm));
        CHECK(arm.y > 0.9f * math::length(arm));
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

TEST_CASE("character validation: starters pass, broken skeletons, sockets and clips are reported") {
    assets::ModelImportSettings settings;
    settings.max_texture_size = 64;
    std::optional<assets::CookedModel> orion;
    for (const auto& c : project::starter_characters()) {
        auto model = assets::import_gltf_memory(project::starter_character_glb(c), c.model_path, settings);
        REQUIRE(model.ok());
        const auto r = project::validate_character(*model);
        for (const auto& i : r.issues) MESSAGE(c.id << ": [" << i.code << "] " << i.message);
        CHECK(r.ok());
        CHECK(r.issues.empty()); // the starters are the reference: no warnings either
        CHECK(r.joints == project::kCharacterJointCount);
        CHECK(r.sockets == project::character_socket_names().size());
        CHECK(r.lods >= 1);
        CHECK(r.height == doctest::Approx(c.height).epsilon(0.01));
        if (c.id == "orion") orion = std::move(model).value();
    }
    REQUIRE(orion);
    auto has = [](const project::CharacterReport& r, std::string_view code, std::string_view text) {
        return std::any_of(r.issues.begin(), r.issues.end(), [&](const project::CharacterIssue& i) {
            return i.severity == project::CharacterIssue::Severity::Error && i.code == code &&
                   i.message.find(text) != std::string::npos;
        });
    };
    auto node = [](assets::CookedModel& m, std::string_view name) -> assets::ModelNode& {
        return *std::find_if(m.nodes.begin(), m.nodes.end(), [&](const auto& n) { return n.name == name; });
    };
    {
        auto m = *orion; // a renamed core joint breaks scripts that look it up
        node(m, "LowerArm.L").name = "Forearm.L";
        const auto r = project::validate_character(m);
        CHECK_FALSE(r.ok());
        CHECK(has(r, "joints", "'LowerArm.L' is missing"));
    }
    {
        auto m = *orion; // a missing socket and a missing controller clip
        node(m, "Socket.Hand.R").name = "Grip";
        std::erase_if(m.animations, [](const animation::Clip& k) { return k.name == "Land"; });
        const auto r = project::validate_character(m);
        CHECK(has(r, "sockets", "Socket.Hand.R"));
        CHECK(has(r, "clips", "'Land'"));
        project::CharacterValidationOptions lax;
        lax.require_sockets = lax.require_controller_clips = false;
        CHECK(project::validate_character(m, lax).ok()); // reported as warnings instead
    }
    {
        auto m = *orion; // centimetre units: 100x too tall
        for (auto& mesh : m.meshes)
            for (auto& v : mesh.data.vertices) v.position = v.position * 100.0f;
        CHECK(has(project::validate_character(m), "scale", "outside"));
    }
    {
        auto m = *orion; // swapped joint order breaks skin-index based retarget maps
        std::swap(m.skins[0].joints[1], m.skins[0].joints[2]);
        std::swap(m.skins[0].inverse_bind[1], m.skins[0].inverse_bind[2]);
        CHECK(has(project::validate_character(m), "joints", "must be joint"));
    }
}

TEST_CASE("starter characters: helmet and bare-head variants (ADR-0247)") {
    using project::CharacterHelmet;
    CHECK(project::parse_character_helmet("auto") == CharacterHelmet::Default);
    CHECK(project::parse_character_helmet("") == CharacterHelmet::Default);
    CHECK(project::parse_character_helmet("on") == CharacterHelmet::On);
    CHECK(project::parse_character_helmet("off") == CharacterHelmet::Off);
    CHECK_FALSE(project::parse_character_helmet("maybe"));
    const auto& orion = *project::find_starter_character("orion");
    const auto& nova = *project::find_starter_character("nova");
    CHECK(orion.default_helmet);      // the sheet's Orion is always helmeted
    CHECK_FALSE(nova.default_helmet); // the sheet's Nova shows her face
    CHECK(project::resolve_character_helmet(nova, CharacterHelmet::On));
    CHECK_FALSE(project::resolve_character_helmet(orion, CharacterHelmet::Off));

    auto visible = [](scene::Scene& s, std::string_view node) {
        const scene::Entity e = s.find_by_name(node);
        REQUIRE_FALSE(e.is_null());
        return s.world().get<scene::MeshRendererComponent>(e)->visible;
    };
    scene::Scene s;
    const scene::Entity root = s.create("Root");
    auto o = project::instantiate_starter_character(s, orion, "O", root);
    auto n = project::instantiate_starter_character(s, nova, "N", root);
    REQUIRE(o.ok());
    REQUIRE(n.ok());
    // Orion: helmet on by default; the Frame and Armor are always drawn.
    CHECK(visible(s, "Orion_Body[0]"));
    CHECK(visible(s, "Orion_Body[1]"));
    CHECK(visible(s, "Orion_Body[2]"));
    CHECK(visible(s, "Orion_Body[3]"));
    CHECK_FALSE(visible(s, "Orion_Body[4]"));
    CHECK_FALSE(visible(s, "Orion_Body[5]"));
    CHECK_FALSE(visible(s, "Orion_Body[6]"));
    CHECK(project::character_helmet(s, *o) == true);
    // Nova: bare head by default.
    CHECK_FALSE(visible(s, "Nova_Body[2]"));
    CHECK_FALSE(visible(s, "Nova_Body[3]"));
    CHECK(visible(s, "Nova_Body[4]"));
    CHECK(visible(s, "Nova_Body[6]"));
    CHECK(project::character_helmet(s, *n) == false);
    // Toggling touches only that character's head primitives, not attachments on its sockets.
    const scene::Entity prop = s.create("Prop");
    auto& pm = s.world().add<scene::MeshRendererComponent>(prop);
    pm.primitive = "cube";
    REQUIRE_FALSE(s.attach_to_socket(prop, *n, "Socket.Hand.R").is_null());
    CHECK(project::set_character_helmet(s, *n, true) == 5);
    CHECK(visible(s, "Nova_Body[3]"));
    CHECK_FALSE(visible(s, "Nova_Body[5]"));
    CHECK(visible(s, "Orion_Body[3]")); // the other character is unchanged
    CHECK(s.world().get<scene::MeshRendererComponent>(prop)->visible);
    CHECK(project::set_character_helmet(s, *n, true) == 0); // idempotent
    CHECK(project::character_helmet(s, *n) == true);
    CHECK(project::set_character_helmet(s, *o, false) == 5);
    CHECK(project::character_helmet(s, *o) == false);
    CHECK_FALSE(project::character_helmet(s, prop)); // no head variants

    // The template option reaches the player character (and survives the scene file).
    project::register_components();
    TempDir dir("victus_tpl_helmet");
    project::TemplateOptions opt;
    opt.character = "nova";
    opt.helmet = "on";
    auto p = project::Project::create(dir.path, "Helmeted", "3d", opt);
    REQUIRE(p.ok());
    scene::Scene loaded;
    REQUIRE(loaded.load_file(*p->startup_scene_path()).ok());
    const scene::Entity robot = loaded.find_by_name("Robot");
    REQUIRE_FALSE(robot.is_null());
    CHECK(project::character_helmet(loaded, robot) == true);
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
        CHECK(project::character_helmet(s, robot) == false); // Nova's default: her bare head
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
