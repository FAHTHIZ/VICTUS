// SPDX-License-Identifier: Apache-2.0
// Skeleton inspection, extension and versioned migration (assets/skeleton.hpp, ADR-0205), tested on
// the real Orion and Nova models: extending them must not change a single deformed vertex of any
// existing pose, keep joint indices, sockets and clips, and pass character validation.
#include "doctest.h"
#include "victus/assets/importers.hpp"
#include "victus/assets/reference_rigs.hpp"
#include "victus/assets/skeleton.hpp"
#include "victus/core/fs.hpp"
#include "victus/core/hash.hpp"
#include "victus/project/characters.hpp"
#include "victus/render/skinning.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>

using namespace victus;
using namespace victus::assets;
using math::Mat4;
using math::Quat;
using math::Vec3;

namespace {

const CookedModel& starter(std::string_view id) {
    static std::map<std::string, CookedModel, std::less<>> cache;
    if (auto it = cache.find(id); it != cache.end()) return it->second;
    const project::StarterCharacter* c = project::find_starter_character(id);
    REQUIRE(c);
    ModelImportSettings s;
    s.import_textures = false; // skeleton and skin only
    s.lods = 0;
    auto m = import_gltf_memory(project::starter_character_glb(*c), c->model_path, s);
    REQUIRE(m.ok());
    return cache.emplace(std::string(id), std::move(m.value())).first->second;
}

const animation::Clip* clip(const CookedModel& m, std::string_view name) {
    for (const auto& c : m.animations)
        if (c.name == name) return &c;
    return nullptr;
}

// Largest vertex difference between two models' skinned body meshes at a clip time.
float max_deformation_delta(const CookedModel& a, const CookedModel& b, std::string_view clip_name, float t,
                            render::SkinningMethod method = render::SkinningMethod::Linear) {
    const auto pa = pose_palette(a, pose_world(a, clip(a, clip_name), t));
    const auto pb = pose_palette(b, pose_world(b, clip(b, clip_name), t));
    float worst = 0;
    std::vector<render::Vertex3D> va, vb;
    for (std::size_t mi = 0; mi < a.meshes.size(); ++mi) {
        if (!a.meshes[mi].data.skinned()) continue;
        render::skin_vertices(a.meshes[mi].data, pa, method, va);
        render::skin_vertices(b.meshes[mi].data, pb, method, vb);
        for (std::size_t i = 0; i < va.size(); ++i) worst = std::max(worst, math::length(va[i].position - vb[i].position));
    }
    return worst;
}

std::vector<BoneEdit> extension_edits() { return complex_humanoid_extension(); }

} // namespace

TEST_CASE("skeleton: Orion's skeleton is described with roles, joints, sockets and one layout hash shared with Nova") {
    const CookedModel& orion = starter("orion");
    const CookedModel& nova = starter("nova");
    const SkeletonInfo info = describe_skeleton(orion);
    CHECK(info.joints == project::kCharacterJointCount);
    CHECK(info.sockets == 8);
    CHECK(info.count(kBoneFinger) == 30);
    CHECK(info.count(kBoneTwist) == 6);
    CHECK(info.count(kBoneEye) == 2);
    CHECK(info.count(kBoneJaw) == 1);
    CHECK(info.count(kBoneToe) == 2);
    CHECK(info.max_depth >= 7);
    const SkeletonBone* hand = info.find("Hand.R");
    REQUIRE(hand);
    CHECK(hand->joint == 12);
    CHECK((hand->roles & kBoneHand) != 0);
    CHECK(hand->weighted_vertices > 0);
    CHECK(hand->animated);
    CHECK(info.find("Socket.Hand.R")->joint == -1);
    CHECK(info.layout_hash != 0);
    CHECK(info.layout_hash == skeleton_layout_hash(nova)); // one layout, two bind poses
    MESSAGE("Orion skeleton: " << info.bones.size() << " bones, " << info.joints << " joints, " << info.sockets << " sockets, "
                               << info.helpers << " helpers, depth " << info.max_depth << ", unweighted joints "
                               << info.unweighted_joints);
    CHECK(bone_roles_string(kBoneArm | kBoneTwist) == "arm|twist");
    CHECK(parse_bone_roles("arm|twist|nonsense") == (kBoneArm | kBoneTwist));
    CHECK((infer_bone_roles("IK.Foot.L") & kBoneIkTarget) != 0);
    CHECK((infer_bone_roles("Mech.Piston.A") & kBoneMechanical) != 0);
    CHECK((infer_bone_roles("Dyn.Tail3") & kBoneSecondary) != 0);
    CHECK((infer_bone_roles("Dyn.Tail3") & kBoneTail) != 0);
}

TEST_CASE("skeleton: extending Orion keeps every existing joint, socket, clip and deformed vertex") {
    const CookedModel& orion = starter("orion");
    CookedModel ext = orion;
    const auto edits = extension_edits();
    SkeletonEditOptions opt;
    for (const char* n : project::character_core_joint_names()) opt.protected_bones.emplace_back(n);
    auto rep = apply_skeleton_edits(ext, edits, opt);
    REQUIRE_MESSAGE(rep.ok(), (rep.ok() ? "" : rep.error().message));
    CHECK(rep->added.size() == edits.size());
    CHECK(rep->vertices_reweighted > 100); // the forearm roll joints took part of the forearms
    CHECK(rep->layout_before == skeleton_layout_hash(orion));
    CHECK(rep->layout_after != rep->layout_before);
    // Existing nodes and joints keep their indices.
    for (std::size_t i = 0; i < orion.nodes.size(); ++i) CHECK(ext.nodes[i].name == orion.nodes[i].name);
    for (std::size_t j = 0; j < orion.skins[0].joints.size(); ++j) CHECK(ext.skins[0].joints[j] == orion.skins[0].joints[j]);
    CHECK(ext.skins[0].joints.size() == orion.skins[0].joints.size() + 14); // 5 IK/pole/socket helpers do not deform
    // Compatibility: only additions.
    const SkeletonCompatibility compat = check_skeleton_compatibility(orion, ext);
    for (const auto& i : compat.issues)
        if (i.severity != SkeletonCompatibilityIssue::Severity::Info) MESSAGE(i.bone << ": " << i.message);
    CHECK(compat.ok());
    CHECK(compat.missing_bones == 0);
    CHECK(compat.added_bones == edits.size());
    // Every pose of every clip deforms exactly as before (the new joints follow their parents
    // until a clip or constraint moves them; moved weights sit on joints with the same matrix).
    float worst = 0;
    for (const auto& c : orion.animations)
        for (const float t : {0.0f, c.duration * 0.37f, c.duration})
            worst = std::max(worst, max_deformation_delta(orion, ext, c.name, t));
    MESSAGE("largest vertex change across " << orion.animations.size() << " clips: " << worst << " m");
    CHECK(worst < 1e-4f);
    // The extended model still validates as a starter character (any joint count).
    project::CharacterValidationOptions vo;
    vo.expect_joint_count = 0;
    const project::CharacterReport cr = project::validate_character(ext, vo);
    for (const auto& i : cr.issues)
        if (i.severity == project::CharacterIssue::Severity::Error) MESSAGE(i.code << ": " << i.message);
    CHECK(cr.ok());
    // The new bones are posable: rolling the forearm roll joint now twists the wrist region.
    const SkeletonInfo info = describe_skeleton(ext);
    const SkeletonBone* roll = info.find("ForeArmRoll.R");
    REQUIRE(roll);
    CHECK(roll->joint >= static_cast<std::int32_t>(project::kCharacterJointCount));
    CHECK(roll->weighted_vertices > 0);
    CHECK((info.find("IK.Hand.L")->roles & kBoneIkTarget) != 0);
    CHECK(info.find("IK.Hand.L")->joint == -1);
    const std::pair<std::uint32_t, Quat> twist[1] = {{static_cast<std::uint32_t>(roll->node), Quat::axis_angle({0, 1, 0}, 1.2f)}};
    const auto rest = pose_palette(ext, pose_world(ext, nullptr, 0));
    const auto rolled = pose_palette(ext, pose_world(ext, nullptr, 0, twist));
    std::vector<render::Vertex3D> v0, v1;
    float moved = 0;
    for (const auto& mesh : ext.meshes) {
        if (!mesh.data.skinned()) continue;
        render::skin_vertices(mesh.data, rest, render::SkinningMethod::DualQuaternion, v0);
        render::skin_vertices(mesh.data, rolled, render::SkinningMethod::DualQuaternion, v1);
        for (std::size_t i = 0; i < v0.size(); ++i) moved = std::max(moved, math::length(v1[i].position - v0[i].position));
    }
    CHECK(moved > 0.005f);
}

TEST_CASE("skeleton: versioned migrations round-trip as JSON, apply to Orion and Nova and refuse other skeletons") {
    const CookedModel& orion = starter("orion");
    const CookedModel& nova = starter("nova");
    SkeletonMigration mig;
    mig.profile = "VictusHumanoid";
    mig.from_version = 1;
    mig.to_version = 2;
    mig.requires_layout = skeleton_layout_hash(orion);
    mig.description = "Facial, toe, roll, IK and secondary bones";
    mig.edits = extension_edits();
    const std::string text = write_skeleton_migration(mig);
    auto parsed = parse_skeleton_migration(text);
    REQUIRE(parsed.ok());
    CHECK(parsed->edits.size() == mig.edits.size());
    CHECK(parsed->requires_layout == mig.requires_layout);
    CHECK(parsed->edits.back().deform == false);
    CHECK(parsed->edits[6].weights_from == "LowerArm.L");
    CHECK(write_skeleton_migration(*parsed) == text);

    for (const CookedModel* src : {&orion, &nova}) {
        CookedModel m = *src;
        auto r = apply_skeleton_migration(m, *parsed);
        REQUIRE(r.ok());
        CHECK(check_skeleton_compatibility(*src, m).ok());
        // Applying the same migration twice is refused: the layout no longer matches.
        auto again = apply_skeleton_migration(m, *parsed);
        CHECK_FALSE(again.ok());
        CHECK(again.error().code == ErrorCode::VersionMismatch);
    }
    // Malformed documents are rejected.
    CHECK_FALSE(parse_skeleton_migration("{\"edits\": [{\"op\": \"explode\", \"bone\": \"x\"}]}").ok());
    CHECK_FALSE(parse_skeleton_migration("{\"from_version\": 3, \"to_version\": 2, \"edits\": []}").ok());
    CHECK_FALSE(parse_skeleton_migration("{\"edits\": [{\"op\": \"add\", \"bone\": \"x\", \"translation\": [1, 2]}]}").ok());
    CHECK_FALSE(parse_skeleton_migration("[]").ok());
}

TEST_CASE("skeleton: unsafe edits are refused and the model is left untouched") {
    const CookedModel& orion = starter("orion");
    SkeletonEditOptions opt;
    for (const char* n : project::character_core_joint_names()) opt.protected_bones.emplace_back(n);
    const auto refuse = [&](BoneEdit e) {
        CookedModel m = orion;
        const BoneEdit one[1] = {e};
        auto r = apply_skeleton_edits(m, one, opt);
        CHECK_FALSE(r.ok());
        CHECK(m.nodes.size() == orion.nodes.size());
        CHECK(skeleton_layout_hash(m) == skeleton_layout_hash(orion));
    };
    refuse({.op = BoneEdit::Op::Add, .bone = "Hand.R", .parent = "LowerArm.R"});   // duplicate
    refuse({.op = BoneEdit::Op::Add, .bone = "Extra", .parent = "NoSuchBone"});    // missing parent
    refuse({.op = BoneEdit::Op::Rename, .bone = "Hand.R", .name = "RightHand"});   // protected core joint
    refuse({.op = BoneEdit::Op::Remove, .bone = "Spine"});                         // protected
    refuse({.op = BoneEdit::Op::Reparent, .bone = "Chest", .parent = "Hand.L"});   // cycle
    refuse({.op = BoneEdit::Op::Rename, .bone = "Jaw", .name = "Head"});           // name taken
    // A forced rename of a core joint is allowed but reported as a broken reference unless the
    // compatibility check is told about the rename.
    CookedModel m = orion;
    const BoneEdit rn[1] = {{.op = BoneEdit::Op::Rename, .bone = "Hand.R", .name = "RightHand", .force = true}};
    auto r = apply_skeleton_edits(m, rn, opt);
    REQUIRE(r.ok());
    CHECK_FALSE(check_skeleton_compatibility(orion, m).ok());
    CompatibilityOptions co;
    co.renames = r->renamed;
    CHECK(check_skeleton_compatibility(orion, m, co).ok());
    CHECK_FALSE(project::validate_character(m).ok()); // the templates need Hand.R
}

TEST_CASE("skeleton: reparenting keeps rest and animated world transforms by re-baking clips") {
    const CookedModel& orion = starter("orion");
    CookedModel m = orion;
    // Move the back socket from Chest to Spine, and the left eye from Head to Jaw.
    const BoneEdit edits[2] = {{.op = BoneEdit::Op::Reparent, .bone = "Socket.Back", .parent = "Spine"},
                               {.op = BoneEdit::Op::Reparent, .bone = "Eye.L", .parent = "Jaw"}};
    auto r = apply_skeleton_edits(m, edits);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    CHECK(r->channels_rebaked > 0);
    const auto find = [](const CookedModel& c, std::string_view n) {
        for (std::size_t i = 0; i < c.nodes.size(); ++i)
            if (c.nodes[i].name == n) return i;
        return std::size_t{0};
    };
    float worst = 0;
    for (const char* clip_name : {"Walk", "Run", "Interact", "Face_Talk", "Idle"})
        for (const float f : {0.0f, 0.21f, 0.5f, 0.77f, 1.0f}) {
            const animation::Clip* ca = clip(orion, clip_name);
            REQUIRE(ca);
            const auto wa = pose_world(orion, ca, ca->duration * f);
            const auto wb = pose_world(m, clip(m, clip_name), ca->duration * f);
            for (const char* bone : {"Socket.Back", "Eye.L"}) {
                const Vec3 pa = wa[find(orion, bone)].c[3].xyz(), pb = wb[find(m, bone)].c[3].xyz();
                worst = std::max(worst, math::length(pa - pb));
            }
        }
    MESSAGE("largest world drift of re-parented bones over five clips: " << worst << " m");
    CHECK(worst < 2e-3f);
    const SkeletonCompatibility compat = check_skeleton_compatibility(orion, m);
    CHECK(compat.ok()); // parents changed (warnings) but nothing moved or vanished
    CHECK(std::any_of(compat.issues.begin(), compat.issues.end(), [](const auto& i) { return i.bone == "Eye.L"; }));
    CHECK(max_deformation_delta(orion, m, "Walk", 0.3f) < 1e-3f);
}

TEST_CASE("skeleton: removing a bone hands its weights on and keeps joint indices stable") {
    const CookedModel& orion = starter("orion");
    CookedModel m = orion;
    const BoneEdit rm[1] = {{.op = BoneEdit::Op::Remove, .bone = "UpperLegTwist.L"}};
    auto r = apply_skeleton_edits(m, rm);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    CHECK(r->vertices_reweighted > 0);
    CHECK(m.nodes.size() == orion.nodes.size() - 1);
    CHECK(m.skins[0].joints.size() == orion.skins[0].joints.size()); // slot kept
    const SkeletonInfo info = describe_skeleton(m);
    CHECK(info.find("UpperLegTwist.L") == nullptr);
    CHECK(info.find("UpperLeg.L")->weighted_vertices > describe_skeleton(orion).find("UpperLeg.L")->weighted_vertices);
    const SkeletonCompatibility compat = check_skeleton_compatibility(orion, m);
    CHECK_FALSE(compat.ok()); // a missing bone is an error for dependants
    CHECK(compat.missing_bones == 1);
    // At rest the deformation is unchanged; the clips did not animate the removed joint.
    CHECK(max_deformation_delta(orion, m, "Idle", 0.0f) < 1e-4f);
    // Compacting renumbers the joints after it, which the compatibility check reports.
    CookedModel c = orion;
    SkeletonEditOptions compact;
    compact.compact_joints = true;
    auto rc = apply_skeleton_edits(c, rm, compact);
    REQUIRE(rc.ok());
    CHECK(c.skins[0].joints.size() == orion.skins[0].joints.size() - 1);
    CHECK(std::count(rc->joint_remap.begin(), rc->joint_remap.end(), -1) == 1);
    CHECK(check_skeleton_compatibility(orion, c).issues.size() > compat.issues.size());
    CHECK(max_deformation_delta(orion, c, "Idle", 0.0f) < 1e-4f);
}

TEST_CASE("skeleton: Orion deforms under extreme poses without collapsing with dual quaternions") {
    // The forearm and thigh rolled 150 degrees and the elbow and knee bent 140 degrees: LBS loses
    // volume around the joints, DQS keeps it. Measured on the real body mesh with the CPU skinner,
    // which the GPU test (gpu_skinning_tests) shows matches the runtime renderer.
    const CookedModel& orion = starter("orion");
    const SkeletonInfo info = describe_skeleton(orion);
    const auto node = [&](const char* n) { return static_cast<std::uint32_t>(info.find(n)->node); };
    const std::pair<std::uint32_t, Quat> pose[4] = {
        {node("LowerArm.R"), Quat::axis_angle({0, 1, 0}, 2.6f) * Quat::axis_angle({1, 0, 0}, 2.4f)},
        {node("Hand.R"), Quat::axis_angle({0, 1, 0}, 1.4f)},
        {node("LowerLeg.L"), Quat::axis_angle({1, 0, 0}, 2.4f)},
        {node("UpperArm.L"), Quat::axis_angle({0, 1, 0}, 2.6f)},
    };
    const auto rest = pose_palette(orion, pose_world(orion, nullptr, 0));
    const auto posed = pose_palette(orion, pose_world(orion, nullptr, 0, pose));
    std::size_t body = 0;
    for (std::size_t i = 0; i < orion.meshes.size(); ++i)
        if (orion.meshes[i].data.skinned() && orion.meshes[i].data.indices.size() > orion.meshes[body].data.indices.size()) body = i;
    const render::MeshData& d = orion.meshes[body].data;
    std::vector<render::Vertex3D> v_rest, v_lbs, v_dqs;
    render::skin_vertices(d, rest, render::SkinningMethod::Linear, v_rest);
    const auto t0 = std::chrono::steady_clock::now();
    render::skin_vertices(d, posed, render::SkinningMethod::Linear, v_lbs);
    const auto t1 = std::chrono::steady_clock::now();
    render::skin_vertices(d, posed, render::SkinningMethod::DualQuaternion, v_dqs);
    const auto t2 = std::chrono::steady_clock::now();
    const render::DeformationMetrics lbs = render::measure_deformation(v_rest, v_lbs, d.indices);
    const render::DeformationMetrics dqs = render::measure_deformation(v_rest, v_dqs, d.indices);
    MESSAGE("Orion body (" << d.vertices.size() << " verts): volume LBS " << lbs.volume_ratio << " DQS " << dqs.volume_ratio
                           << ", collapsed tris LBS " << lbs.collapsed_triangles << " DQS " << dqs.collapsed_triangles
                           << ", min area ratio LBS " << lbs.min_area_ratio << " DQS " << dqs.min_area_ratio
                           << ", flipped LBS " << lbs.flipped_triangles << " DQS " << dqs.flipped_triangles
                           << "; CPU skin LBS " << std::chrono::duration<double, std::milli>(t1 - t0).count() << " ms, DQS "
                           << std::chrono::duration<double, std::milli>(t2 - t1).count() << " ms");
    CHECK(dqs.collapsed_triangles <= lbs.collapsed_triangles);
    CHECK(std::abs(dqs.volume_ratio - 1.0f) <= std::abs(lbs.volume_ratio - 1.0f) + 1e-3f);
}

TEST_CASE("skeleton: model import settings apply migrations from files and fail loudly on a mismatch") {
    const project::StarterCharacter* c = project::find_starter_character("orion");
    REQUIRE(c);
    const fs::Path dir = std::filesystem::temp_directory_path() / ("victus_skel_mig_" + Uuid::generate().to_string().substr(0, 8));
    std::filesystem::create_directories(dir / "rigs");
    struct Cleanup {
        fs::Path p;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(p, ec);
        }
    } cleanup{dir};
    const auto glb = project::starter_character_glb(*c);
    REQUIRE(fs::write_atomic(dir / "orion.glb", glb).ok());
    SkeletonMigration mig;
    mig.profile = "VictusHumanoid";
    mig.requires_layout = skeleton_layout_hash(starter("orion"));
    mig.edits = extension_edits();
    REQUIRE(fs::write_text_atomic(dir / "rigs" / "orion_v2.json", write_skeleton_migration(mig)).ok());

    ModelImportSettings s;
    s.import_textures = false;
    s.lods = 0;
    s.skeleton_migrations = {"rigs/orion_v2.json"};
    // The setting round-trips through the .meta JSON.
    CHECK(ModelImportSettings::from_json(s.to_json()).skeleton_migrations == s.skeleton_migrations);
    CHECK(ModelImportSettings{}.to_json().find("skeleton_migrations") == nullptr);
    ImportResult res;
    auto m = import_gltf(dir / "orion.glb", s, dir, &res);
    REQUIRE_MESSAGE(m.ok(), (m.ok() ? "" : m.error().message));
    CHECK(describe_skeleton(*m).find("ForeArmRoll.L"));
    CHECK(check_skeleton_compatibility(starter("orion"), *m).ok());
    CHECK(std::any_of(res.dependencies.begin(), res.dependencies.end(),
                      [](const fs::Path& p) { return p.filename() == "orion_v2.json"; }));
    // Listing it twice fails: the second copy no longer matches the layout.
    s.skeleton_migrations = {"rigs/orion_v2.json", "rigs/orion_v2.json"};
    auto twice = import_gltf(dir / "orion.glb", s, dir);
    REQUIRE_FALSE(twice.ok());
    CHECK(twice.error().message.find("orion_v2.json") != std::string::npos);
    // Paths outside the sandbox and missing files are refused.
    s.skeleton_migrations = {"../elsewhere.json"};
    CHECK_FALSE(import_gltf(dir / "orion.glb", s, dir).ok());
    s.skeleton_migrations = {"rigs/missing.json"};
    CHECK_FALSE(import_gltf(dir / "orion.glb", s, dir).ok());
    // Memory imports cannot read files: they warn and import the original skeleton.
    ImportResult mem_res;
    auto mem = import_gltf_memory(glb, "orion.glb", s, &mem_res);
    REQUIRE(mem.ok());
    CHECK(mem_res.warnings.size() == 1);
    CHECK(describe_skeleton(*mem).find("ForeArmRoll.L") == nullptr);
}
