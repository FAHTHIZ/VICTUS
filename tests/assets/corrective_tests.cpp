// SPDX-License-Identifier: Apache-2.0
// Corrective shapes driven by joint angles (assets/correctives.hpp, ADR-0220), measured on Orion:
// a baked corrective makes linear blend skinning reproduce dual quaternion skinning at its key
// pose, blends in on the way there, and is absent at rest.
#include "doctest.h"
#include "victus/assets/correctives.hpp"
#include "victus/assets/reference_rigs.hpp"
#include "victus/assets/skeleton.hpp"
#include "victus/project/characters.hpp"
#include "victus/render/skinning.hpp"

#include <chrono>
#include <cmath>

using namespace victus;
using namespace victus::assets;
using math::Quat;
using math::Vec3;

namespace {

constexpr float kDeg = 3.14159265f / 180.0f;

const CookedModel& orion() {
    static const CookedModel model = [] {
        const project::StarterCharacter* c = project::find_starter_character("orion");
        REQUIRE(c);
        ModelImportSettings s;
        s.import_textures = false;
        s.lods = 0;
        auto m = import_gltf_memory(project::starter_character_glb(*c), c->model_path, s);
        REQUIRE(m.ok());
        return std::move(m.value());
    }();
    return model;
}

std::uint32_t node_of(const CookedModel& m, std::string_view name) {
    for (std::size_t i = 0; i < m.nodes.size(); ++i)
        if (m.nodes[i].name == name) return static_cast<std::uint32_t>(i);
    FAIL("missing node " << std::string(name));
    return 0;
}

struct PoseResult {
    std::size_t collapsed{0};
    float volume{1};     // local volume ratio around the joint (worst mesh)
    float max_error{0};  // largest distance from the DQS result (metres)
};

// Every skinned mesh of `m` with `joint` turned `degrees` about `axis`, skinned with LBS after
// applying the mesh's morph targets at the weights the correctives give for that pose (`use`
// false: no correctives), compared with plain DQS.
PoseResult measure(const CookedModel& m, std::string_view joint, Vec3 axis, float degrees, bool use, render::SkinningMethod method = render::SkinningMethod::Linear) {
    const std::uint32_t node = node_of(m, joint);
    const Quat local = m.nodes[node].rotation * Quat::axis_angle(axis, degrees * kDeg);
    const std::pair<std::uint32_t, Quat> pose[1] = {{node, local}};
    const auto rest = pose_palette(m, pose_world(m, nullptr, 0));
    const auto posed = pose_palette(m, pose_world(m, nullptr, 0, pose));
    std::uint32_t joint_index = 0;
    for (std::size_t j = 0; j < m.skins[0].joints.size(); ++j)
        if (m.skins[0].joints[j] == node) joint_index = static_cast<std::uint32_t>(j);
    PoseResult r;
    r.volume = 10;
    std::vector<render::Vertex3D> base, v0, v1, ref;
    std::vector<float> weights;
    for (const auto& mesh : m.meshes) {
        if (!mesh.data.skinned()) continue;
        const auto rot = [&](std::uint32_t n) { return n == node ? local : m.nodes[n].rotation; };
        corrective_mesh_weights(m, m.skins[0], mesh.data, mesh.data.morph_weights, rot, weights);
        if (!use) std::fill(weights.begin(), weights.end(), 0.0f);
        render::apply_morphs(mesh.data, weights, base);
        render::skin_vertices(mesh.data, rest, method, v0); // the unmorphed rest shape
        render::skin_vertices(mesh.data, base, posed, method, v1);
        render::skin_vertices(mesh.data, posed, render::SkinningMethod::DualQuaternion, ref);
        r.collapsed += render::measure_deformation(v0, v1, mesh.data.indices).collapsed_triangles;
        const std::uint32_t js[1] = {joint_index};
        bool influenced = false;
        for (const auto& sv : mesh.data.skin)
            for (int k = 0; k < 4; ++k) influenced |= sv.joints[k] == joint_index && sv.weights[k] >= 0.05f;
        if (influenced) r.volume = std::min(r.volume, render::local_volume_ratio(mesh.data, v1, js));
        for (std::size_t i = 0; i < v1.size(); ++i) r.max_error = std::max(r.max_error, math::length(v1[i].position - ref[i].position));
    }
    return r;
}

std::size_t morph_count(const CookedModel& m) {
    std::size_t n = 0;
    for (const auto& mesh : m.meshes) n += mesh.data.morphs.size();
    return n;
}

} // namespace

TEST_CASE("correctives: the driver angle is the twist about the axis and the weight is a smoothstep") {
    const Quat rest = math::normalize(Quat::axis_angle(math::normalize(Vec3{0.3f, 1, -0.2f}), 0.9f));
    CHECK(corrective_angle(rest, rest, {0, 1, 0}) == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(corrective_angle(rest, rest * Quat::axis_angle({0, 1, 0}, 0.7f), {0, 1, 0}) == doctest::Approx(0.7f).epsilon(1e-4));
    CHECK(corrective_angle(rest, rest * Quat::axis_angle({0, 1, 0}, -1.2f), {0, 1, 0}) == doctest::Approx(-1.2f).epsilon(1e-4));
    CHECK(std::abs(corrective_angle(rest, rest * Quat::axis_angle({1, 0, 0}, 0.8f), {0, 1, 0})) < 1e-5f);
    // Swing about X on top of the twist does not change the measured twist.
    CHECK(corrective_angle(rest, rest * Quat::axis_angle({1, 0, 0}, 0.5f) * Quat::axis_angle({0, 1, 0}, 0.7f), {0, 1, 0}) ==
          doctest::Approx(0.7f).epsilon(1e-4));
    // An unnormalised axis and a negated quaternion (same rotation) give the same angle.
    CHECK(corrective_angle(rest, rest * Quat::axis_angle({0, 1, 0}, 0.7f), {0, 3, 0}) == doctest::Approx(0.7f).epsilon(1e-4));
    const Quat q = rest * Quat::axis_angle({0, 1, 0}, 0.7f);
    CHECK(corrective_angle(rest, Quat{-q.x, -q.y, -q.z, -q.w}, {0, 1, 0}) == doctest::Approx(0.7f).epsilon(1e-4));

    ModelCorrective c;
    c.axis = {0, 1, 0};
    c.start = 0;
    c.end = 2.0f;
    const auto w = [&](float a) { return corrective_weight(c, rest, rest * Quat::axis_angle({0, 1, 0}, a)); };
    CHECK(w(0) == 0.0f);
    CHECK(w(-0.5f) == 0.0f);
    CHECK(w(1.0f) == doctest::Approx(0.5f).epsilon(1e-4));
    CHECK(w(2.0f) == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(w(2.5f) == 1.0f);
    c.end = -2.0f; // drives the negative direction
    CHECK(w(1.0f) == 0.0f);
    CHECK(w(-1.0f) == doctest::Approx(0.5f).epsilon(1e-4));
    // An in-between: rises to 1 at end, fades back to 0 at release.
    c.end = 2.0f;
    c.release = 3.0f;
    CHECK(w(2.5f) == doctest::Approx(0.5f).epsilon(1e-4));
    CHECK(w(3.5f) == 0.0f);
}

TEST_CASE("correctives: a baked forearm-twist corrective makes LBS match DQS at its pose and blends in on the way") {
    CookedModel m = orion();
    CorrectiveSettings s;
    s.joint = "LowerArm.R";
    s.axis = {0, 1, 0}; // along the bone: twist
    s.degrees = 150;
    const auto t0 = std::chrono::steady_clock::now();
    auto r = add_corrective(m, s);
    const double bake_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    CHECK(r->baked);
    CHECK(r->morph == "Corrective.LowerArm.R.150");
    REQUIRE(r->morphs.size() == 3); // in-betweens at 50 and 100 degrees
    CHECK(r->morphs[0] == "Corrective.LowerArm.R.50");
    CHECK(r->vertices > 0);
    CHECK(r->max_residual < 1e-4f);
    REQUIRE(m.skins[0].correctives.size() == 3);
    CHECK(validate_model(m).ok());
    const std::size_t bytes = r->vertices * (sizeof(std::uint32_t) + 2 * sizeof(Vec3));
    MESSAGE("forearm twist corrective: " << r->vertices << " vertices on " << r->meshes << " meshes, max offset " << r->max_offset * 100
                                         << " cm, residual " << r->max_residual * 1000 << " mm, bake " << bake_ms << " ms, " << bytes << " B");

    // At the key pose LBS + corrective is DQS; plain LBS candy-wraps.
    const PoseResult lbs = measure(m, s.joint, s.axis, 150, false);
    const PoseResult fixed = measure(m, s.joint, s.axis, 150, true);
    const PoseResult dqs = measure(m, s.joint, s.axis, 150, false, render::SkinningMethod::DualQuaternion);
    MESSAGE("twist 150: collapsed LBS " << lbs.collapsed << ", LBS+corrective " << fixed.collapsed << ", DQS " << dqs.collapsed
                                        << "; local volume " << lbs.volume << " / " << fixed.volume << " / " << dqs.volume
                                        << "; max distance from DQS " << lbs.max_error * 100 << " cm / " << fixed.max_error * 100 << " cm");
    CHECK(lbs.collapsed > 0);
    CHECK(fixed.collapsed == dqs.collapsed);
    CHECK(fixed.max_error < 1e-4f);
    CHECK(lbs.max_error > 0.005f);
    // Exact at the in-between keys; between keys two shapes cross-fade, closer to DQS than plain
    // LBS though not exact.
    CHECK(measure(m, s.joint, s.axis, 50, true).max_error < 1e-4f);
    CHECK(measure(m, s.joint, s.axis, 100, true).max_error < 1e-4f);
    for (const float a : {25.0f, 75.0f, 125.0f}) {
        const PoseResult between_lbs = measure(m, s.joint, s.axis, a, false);
        const PoseResult between_fix = measure(m, s.joint, s.axis, a, true);
        MESSAGE("twist " << a << ": max distance from DQS LBS " << between_lbs.max_error * 100 << " cm, LBS+corrective "
                         << between_fix.max_error * 100 << " cm; collapsed " << between_lbs.collapsed << " / " << between_fix.collapsed);
        CHECK(between_fix.collapsed <= between_lbs.collapsed);
        // Below the first key (where LBS is still close to DQS) the half-weighted shape overshoots
        // slightly (measured 1.5 mm against 1.3 mm at 25 degrees); further in it is clearly better.
        if (a < 50.0f) CHECK(between_fix.max_error < between_lbs.max_error + 0.0005f);
        else CHECK(between_fix.max_error < 0.5f * between_lbs.max_error);
    }
    // Per-frame CPU cost of the correctives at 75 degrees: driver weights plus morphing every mesh
    // that carries them (the renderer does this only while a weight changes).
    {
        const std::uint32_t node = node_of(m, s.joint);
        const Quat local = m.nodes[node].rotation * Quat::axis_angle(s.axis, 75 * kDeg);
        const auto rot = [&](std::uint32_t n) { return n == node ? local : m.nodes[n].rotation; };
        std::vector<render::Vertex3D> out;
        std::vector<float> w;
        std::size_t morphed = 0;
        constexpr int kRuns = 20;
        const auto c0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kRuns; ++i)
            for (const auto& mesh : m.meshes) {
                if (!mesh.data.skinned()) continue;
                if (corrective_mesh_weights(m, m.skins[0], mesh.data, mesh.data.morph_weights, rot, w) == 0) continue;
                (void)render::apply_morphs(mesh.data, w, out);
                morphed += out.size();
            }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count() / kRuns;
        MESSAGE("corrective cost per frame at 75 deg: " << ms << " ms CPU, " << morphed / kRuns << " vertices morphed");
        CHECK(morphed > 0);
    }
    // At rest the weight is 0 and nothing changes; twisting the other way leaves it at 0 too.
    CHECK(measure(m, s.joint, s.axis, 0, true).max_error == measure(m, s.joint, s.axis, 0, false).max_error);
    CHECK(measure(m, s.joint, s.axis, -60, true).max_error == measure(m, s.joint, s.axis, -60, false).max_error);
}

TEST_CASE("correctives: a knee-bend corrective, an authored morph driven by a joint, and refused settings") {
    CookedModel m = orion();
    CorrectiveSettings knee;
    knee.joint = "LowerLeg.L";
    knee.axis = {1, 0, 0};
    knee.degrees = 115;
    knee.start_degrees = 20;
    auto r = add_corrective(m, knee);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    const PoseResult lbs = measure(m, knee.joint, knee.axis, 115, false);
    const PoseResult fixed = measure(m, knee.joint, knee.axis, 115, true);
    const PoseResult dqs = measure(m, knee.joint, knee.axis, 115, false, render::SkinningMethod::DualQuaternion);
    MESSAGE("knee bend 115: " << r->vertices << " vertices; collapsed LBS " << lbs.collapsed << ", LBS+corrective " << fixed.collapsed
                              << ", DQS " << dqs.collapsed << "; local volume " << lbs.volume << " / " << fixed.volume << " / " << dqs.volume);
    CHECK(fixed.collapsed == dqs.collapsed);
    CHECK(fixed.max_error < 1e-4f);
    CHECK(measure(m, knee.joint, knee.axis, 15, true).max_error == measure(m, knee.joint, knee.axis, 15, false).max_error);

    // An existing morph target driven by a joint (a facial shape on the jaw here).
    std::string authored;
    for (const auto& mesh : m.meshes)
        if (mesh.data.skinned() && !mesh.data.morphs.empty() && authored.empty()) authored = mesh.data.morphs.front().name;
    REQUIRE_FALSE(authored.empty());
    CorrectiveSettings drive;
    drive.joint = "Head";
    drive.morph = authored;
    drive.degrees = 30;
    auto d = add_corrective(m, drive);
    REQUIRE_MESSAGE(d.ok(), (d.ok() ? "" : d.error().message));
    CHECK_FALSE(d->baked);
    CHECK(d->meshes >= 1);
    CHECK(m.skins[0].correctives.size() == 4); // three knee shapes and the driven one

    // Refused: the model is unchanged.
    const auto refuse = [&](CorrectiveSettings bad, CookedModel& model) {
        const std::size_t morphs = morph_count(model), drivers = model.skins[0].correctives.size();
        auto res = add_corrective(model, bad);
        CHECK_FALSE(res.ok());
        CHECK(morph_count(model) == morphs);
        CHECK(model.skins[0].correctives.size() == drivers);
    };
    CorrectiveSettings bad = knee;
    refuse(knee, m); // same name again
    bad.joint = "Socket.Hand.R"; // not a skin joint
    refuse(bad, m);
    bad = knee;
    bad.axis = {0, 0, 0};
    refuse(bad, m);
    bad = knee;
    bad.start_degrees = bad.degrees;
    refuse(bad, m);
    bad = knee;
    bad.degrees = 270;
    refuse(bad, m);
    bad = knee;
    bad.degrees = 100;
    bad.target = "smooth";
    refuse(bad, m);
    bad = drive;
    bad.morph = "NoSuchShape";
    refuse(bad, m);
    bad = knee;
    bad.skin = 3;
    CHECK_FALSE(add_corrective(m, bad).ok());
    // A rigid rig has nothing to correct: LBS and DQS agree on single-influence vertices.
    CookedModel mech = make_reference_rig(ReferenceRig::Mech);
    CorrectiveSettings arm;
    arm.joint = "Mech.Boom";
    arm.degrees = 60;
    refuse(arm, mech);
}

TEST_CASE("correctives: drivers survive cooking, settings round-trip as JSON and import bakes them") {
    CookedModel m = orion();
    CorrectiveSettings s;
    s.joint = "LowerArm.L";
    s.axis = {0, 1, 0};
    s.degrees = -140;
    s.start_degrees = -10;
    REQUIRE(add_corrective(m, s).ok());
    const auto bytes = serialize(CookedHeader{.kind = AssetKind::Model}, m);
    auto back = deserialize_model(bytes);
    REQUIRE_MESSAGE(back.ok(), (back.ok() ? "" : back.error().message));
    REQUIRE(back->skins[0].correctives.size() == 3);
    const ModelCorrective& first = back->skins[0].correctives[0];
    CHECK(first.morph == "Corrective.LowerArm.L.-53.3");
    CHECK(first.start == doctest::Approx(-10 * kDeg));
    CHECK(first.release == doctest::Approx(-96.6667f * kDeg));
    const ModelCorrective& c = back->skins[0].correctives[2];
    CHECK(c.morph == "Corrective.LowerArm.L.-140");
    CHECK(c.end == doctest::Approx(-140 * kDeg));
    CHECK(c.release == c.end);
    CHECK(m.skins[0].joints[c.joint] == node_of(m, "LowerArm.L"));
    // A corrupt driver is caught by validation.
    CookedModel broken = m;
    broken.skins[0].correctives[0].joint = 9999;
    CHECK_FALSE(validate_model(broken).ok());

    ModelImportSettings settings;
    settings.import_textures = false;
    settings.lods = 0;
    settings.correctives = {s, CorrectiveSettings{.joint = "Head", .axis = {0, 1, 0}, .degrees = 40, .start_degrees = 0, .morph = "x"}};
    const json::Value j = settings.to_json();
    const ModelImportSettings parsed = ModelImportSettings::from_json(j);
    REQUIRE(parsed.correctives.size() == 2);
    CHECK(parsed.correctives[0] == s);
    CHECK(parsed.correctives[1].morph == "x");
    // Malformed entries are dropped by the settings parser.
    json::Value bad = j;
    auto arr = json::Value::array();
    arr.push(json::Value::object());
    bad.set("correctives", std::move(arr));
    CHECK(ModelImportSettings::from_json(bad).correctives.empty());

    // Import with one corrective: the model arrives with the morph and the driver; a broken one
    // fails the import loudly.
    const project::StarterCharacter* oc = project::find_starter_character("orion");
    settings.correctives = {s};
    auto imported = import_gltf_memory(project::starter_character_glb(*oc), oc->model_path, settings);
    REQUIRE_MESSAGE(imported.ok(), (imported.ok() ? "" : imported.error().message));
    CHECK(imported->skins[0].correctives.size() == 3);
    CHECK(morph_count(*imported) == morph_count(m));
    settings.correctives[0].joint = "NoSuchBone";
    CHECK_FALSE(import_gltf_memory(project::starter_character_glb(*oc), oc->model_path, settings).ok());
}
