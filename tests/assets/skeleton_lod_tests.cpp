// SPDX-License-Identifier: Apache-2.0
// Skeleton LOD (ADR-0221): detail nodes of Orion, the reference rigs and the face morphs are
// tagged by role; an animator past its detail tier eases them to rest without a pop, stops
// sampling them, keeps every body joint identical to a full-detail animator, and eases them back.
#include "doctest.h"

#include "victus/animation/animation.hpp"
#include "victus/assets/cooked.hpp"
#include "victus/assets/importers.hpp"
#include "victus/assets/reference_rigs.hpp"
#include "victus/assets/skeleton.hpp"
#include "victus/gameplay/sim_lod.hpp"
#include "victus/project/characters.hpp"
#include "victus/render/skinning.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>

using namespace victus;
using namespace victus::assets;
using math::Quat;
using math::Vec3;

namespace {

const CookedModel& orion_model() {
    static const CookedModel model = [] {
        const project::StarterCharacter* c = project::find_starter_character("orion");
        REQUIRE(c);
        ModelImportSettings s;
        s.import_textures = false;
        s.lods = 0;
        auto m = import_gltf_memory(project::starter_character_glb(*c), c->model_path, s);
        REQUIRE(m.ok());
        return std::move(m).value();
    }();
    return model;
}

// Angle of a^-1 b, precise near zero (acos of the dot product is not).
float angle_between(Quat a, Quat b) {
    const Quat r = a.conjugate() * b;
    return 2.0f * std::atan2(std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z), std::abs(r.w));
}

struct Rig {
    scene::Scene scene;
    std::vector<scene::Entity> roots;
    std::unique_ptr<animation::AnimationSystem> sys;
    std::string controller = project::starter_character_controller();
    std::shared_ptr<animation::AnimationSet> set;
    Uuid id;
};

// `n` Orions under the shared controller, walking-to-running; `tier` = detail_lod_tier of each.
void build(Rig& r, int n, std::int32_t tier) {
    const auto* c = project::find_starter_character("orion");
    REQUIRE(Uuid::parse(c->uuid, r.id));
    r.set = std::make_shared<animation::AnimationSet>(make_animation_set(orion_model()));
    for (int i = 0; i < n; ++i) {
        auto e = project::instantiate_starter_character(r.scene, *c, "Robot", scene::kNullEntity);
        REQUIRE(e.ok());
        auto* a = r.scene.world().get<animation::AnimatorComponent>(*e);
        a->time = 0.01f * static_cast<float>(i);
        a->detail_lod_tier = tier;
        r.roots.push_back(*e);
    }
    animation::AnimationSettings settings;
    settings.controllers = [&r](std::string_view) -> Result<std::string> { return r.controller; };
    r.sys = std::make_unique<animation::AnimationSystem>(settings);
    r.sys->add_set(r.id, r.set);
    r.sys->update(r.scene, 0.0f);
    for (const auto e : r.roots)
        (void)r.sys->set_parameter(r.scene, e, "speed", 3.5f);
}

// Local rotations of the named nodes under root `i` of the rig.
std::vector<Quat> rotations(Rig& r, std::size_t i, const std::vector<std::string>& names) {
    std::vector<Quat> out;
    for (const auto& n : names) {
        Quat q{};
        std::vector<scene::Entity> stack{r.roots[i]};
        while (!stack.empty()) {
            const scene::Entity e = stack.back();
            stack.pop_back();
            if (const auto* nm = r.scene.world().get<scene::Name>(e); nm && nm->value == n) {
                q = r.scene.world().get<scene::Transform>(e)->rotation;
                break;
            }
            for (const auto ch : r.scene.children(e))
                stack.push_back(ch);
        }
        out.push_back(q);
    }
    return out;
}

} // namespace

TEST_CASE("skeleton lod: detail nodes are tagged by role on Orion, the face morphs and the reference rigs") {
    const animation::AnimationSet set = make_animation_set(orion_model());
    std::size_t detail = 0, morphs = 0, joints = 0;
    auto lod_of = [&](std::string_view name) {
        for (const auto& n : set.nodes)
            if (n.name == name) return static_cast<int>(n.lod);
        return -1;
    };
    for (const auto& n : set.nodes) {
        detail += n.lod;
        if (n.is_morph())
            morphs += n.lod;
        else
            joints += n.lod;
    }
    MESSAGE("Orion: " << set.nodes.size() << " animation nodes, " << joints << " detail joints, " << morphs
                      << " detail morph weights");
    CHECK(lod_of("Hand.R") == 0);
    CHECK(lod_of("Hips") == 0);
    CHECK(lod_of("Head") == 0);
    CHECK(lod_of("LowerArm.R") == 0);
    CHECK(lod_of("Socket.Hand.R") == 0);
    CHECK(lod_of("Jaw") == 1);
    CHECK(lod_of("Eye.L") == 1);
    CHECK(lod_of("Index1.R") == 1);
    CHECK(lod_of("Toe.L") == 1);
    CHECK(joints >= 30); // 30 finger joints, toes, jaw, eyes
    CHECK(morphs >= 26); // the face display's ARKit shapes
    // Quadruped: the jaw and the jiggle belly are detail; ears and the tail carry the silhouette.
    const animation::AnimationSet quad = make_animation_set(make_reference_rig(ReferenceRig::Quadruped));
    std::size_t qd = 0;
    for (const auto& n : quad.nodes)
        qd += n.lod;
    // Mech: every part is mechanical, nothing is detail.
    const animation::AnimationSet mech = make_animation_set(make_reference_rig(ReferenceRig::Mech));
    std::size_t md = 0;
    for (const auto& n : mech.nodes)
        md += n.lod;
    MESSAGE("quadruped detail nodes " << qd << " of " << quad.nodes.size() << ", mech " << md << " of "
                                      << mech.nodes.size());
    CHECK(qd >= 1);
    CHECK(md == 0);
}

TEST_CASE("skeleton lod: Orion's detail joints ease to rest without a pop, stop sampling, and the body is unchanged") {
    Rig full, lod;
    build(full, 1, -1);
    build(lod, 1, 0); // reduced from tier 0: always, for the test
    const std::vector<std::string> body{"Hips",       "Spine",  "Head",       "UpperArm.L",
                                        "LowerArm.R", "Hand.R", "UpperLeg.L", "Foot.R"};
    std::vector<std::string> fingers;
    for (const auto& n : full.set->nodes)
        if (n.lod && !n.is_morph()) fingers.push_back(n.name);
    REQUIRE(fingers.size() >= 30);
    std::vector<Quat> rest;
    for (const auto& f : fingers)
        for (const auto& n : full.set->nodes)
            if (n.name == f) rest.push_back(n.rest.rotation);

    constexpr float kDt = 1.0f / 60;
    float max_step_lod = 0, max_step_full = 0, max_offset = 0, body_error = 0;
    std::vector<Quat> prev_lod = rotations(lod, 0, fingers), prev_full = rotations(full, 0, fingers);
    std::uint32_t channels_full = 0, channels_lod = 0;
    for (int f = 0; f < 40; ++f) {
        full.sys->update(full.scene, kDt);
        lod.sys->update(lod.scene, kDt);
        channels_full = full.sys->stats().channels_sampled;
        channels_lod = lod.sys->stats().channels_sampled;
        const auto a = rotations(full, 0, fingers), b = rotations(lod, 0, fingers);
        for (std::size_t i = 0; i < fingers.size(); ++i) {
            max_step_lod = std::max(max_step_lod, angle_between(b[i], prev_lod[i]));
            max_step_full = std::max(max_step_full, angle_between(a[i], prev_full[i]));
            if (f == 0)
                max_offset = std::max(max_offset, angle_between(a[i], rest[i])); // the pop an instant switch would make
        }
        prev_lod = b;
        prev_full = a;
        const auto ba = rotations(full, 0, body), bb = rotations(lod, 0, body);
        for (std::size_t i = 0; i < body.size(); ++i)
            body_error = std::max(body_error, angle_between(ba[i], bb[i]));
    }
    constexpr float kRad = 180.0f / 3.14159265f;
    MESSAGE("detail joints " << fingers.size() << ": largest per-frame turn while easing out " << max_step_lod * kRad
                             << " deg (full detail " << max_step_full * kRad << " deg; an instant switch would jump "
                             << max_offset * kRad << " deg); channels sampled " << channels_full << " -> "
                             << channels_lod << "; body joint difference " << body_error * kRad << " deg");
    CHECK(body_error < 1e-4f);
    CHECK(lod.sys->stats().detail_reduced == 1);
    CHECK(full.sys->stats().detail_reduced == 0);
    CHECK(channels_lod < channels_full);
    // After the fade (0.3 s) the detail joints sit exactly at rest.
    const auto settled = rotations(lod, 0, fingers);
    for (std::size_t i = 0; i < fingers.size(); ++i)
        CHECK(angle_between(settled[i], rest[i]) < 1e-4f);
    // Easing out moves no joint more per frame than the fade allows on top of the animation.
    CHECK(max_step_lod <= max_step_full + max_offset * (kDt / 0.3f) + 1e-3f);

    // Back to full detail: the joints ease back to the animation and then match it.
    lod.scene.world().get<animation::AnimatorComponent>(lod.roots[0])->detail_lod_tier = -1;
    for (int f = 0; f < 30; ++f) {
        full.sys->update(full.scene, kDt);
        lod.sys->update(lod.scene, kDt);
    }
    const auto a = rotations(full, 0, fingers), b = rotations(lod, 0, fingers);
    float back = 0;
    for (std::size_t i = 0; i < fingers.size(); ++i)
        back = std::max(back, angle_between(a[i], b[i]));
    CHECK(back < 1e-4f);
    CHECK(lod.sys->stats().detail_reduced == 0);
}

TEST_CASE("skeleton lod: the simulation LOD tier drives it, and the vertex error and CPU saving are measured") {
    Rig r;
    build(r, 1, 2);
    const scene::Entity src = r.scene.create("Camera");
    r.scene.world().add<gameplay::SimulationLodSource>(src);
    r.scene.world().add<gameplay::SimulationLodComponent>(r.roots[0]);
    r.scene.world().get<scene::Transform>(r.roots[0])->position = {0, 0, 120}; // tier 2 (80..200 m)
    gameplay::FrameTime t;
    t.dt = 1.0f / 60;
    for (int f = 0; f < 120; ++f) {
        ++t.frame;
        t.time += t.dt;
        r.scene.update_transforms();
        gameplay::update_simulation_lod(r.scene, t);
        r.sys->update(r.scene, t.dt);
    }
    CHECK(r.scene.world().get<gameplay::SimulationLodComponent>(r.roots[0])->tier == 2);
    CHECK(r.sys->stats().detail_reduced + r.sys->stats().lod_skipped >= 1);
    // Near (tier 0) it comes back.
    r.scene.world().get<scene::Transform>(r.roots[0])->position = {0, 0, 5};
    for (int f = 0; f < 60; ++f) {
        ++t.frame;
        t.time += t.dt;
        r.scene.update_transforms();
        gameplay::update_simulation_lod(r.scene, t);
        r.sys->update(r.scene, t.dt);
    }
    CHECK(r.sys->stats().detail_reduced == 0);

    // How far do the vertices move when the detail is at rest instead of animated? Skin Orion with
    // a full and a reduced pose taken from the same moment of the walk-run blend.
    Rig full, lod;
    build(full, 1, -1);
    build(lod, 1, 0);
    for (int f = 0; f < 30; ++f) {
        full.sys->update(full.scene, 1.0f / 60);
        lod.sys->update(lod.scene, 1.0f / 60);
    }
    const CookedModel& m = orion_model();
    auto pose_of = [&](Rig& rig) {
        std::vector<std::string> names;
        for (const auto& n : m.nodes)
            names.push_back(n.name);
        const auto rots = rotations(rig, 0, names);
        std::vector<std::pair<std::uint32_t, Quat>> ov;
        for (std::size_t i = 0; i < names.size(); ++i)
            ov.emplace_back(static_cast<std::uint32_t>(i), rots[i]);
        ov.erase(ov.begin()); // the root keeps the model's own transform
        return pose_palette(m, pose_world(m, nullptr, 0, ov));
    };
    const auto pa = pose_of(full), pb = pose_of(lod);
    float max_move = 0;
    std::size_t moved = 0, total = 0;
    for (const auto& mesh : m.meshes) {
        if (!mesh.data.skinned()) continue;
        std::vector<render::Vertex3D> va, vb;
        render::skin_vertices(mesh.data, pa, render::SkinningMethod::Linear, va);
        render::skin_vertices(mesh.data, pb, render::SkinningMethod::Linear, vb);
        for (std::size_t i = 0; i < va.size(); ++i) {
            const float d = math::length(va[i].position - vb[i].position);
            max_move = std::max(max_move, d);
            moved += d > 1e-4f ? 1 : 0;
        }
        total += va.size();
    }
    MESSAGE("reduced pose: " << moved << " of " << total << " vertices move, at most " << max_move * 100 << " cm");
    CHECK(moved < total / 4); // only hands, feet and face
    CHECK(max_move < 0.10f);

    // CPU: 64 Orions, full against reduced (median of 15 updates each).
    auto time_update = [](Rig& rig) {
        std::vector<double> v;
        for (int i = 0; i < 15; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            rig.sys->update(rig.scene, 1.0f / 60);
            v.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    Rig many_full, many_lod;
    build(many_full, 64, -1);
    build(many_lod, 64, 0);
    for (int f = 0; f < 30; ++f) {
        many_full.sys->update(many_full.scene, 1.0f / 60);
        many_lod.sys->update(many_lod.scene, 1.0f / 60);
    }
    const double ms_full = time_update(many_full), ms_lod = time_update(many_lod);
    MESSAGE("64 Orions, animation update: full detail " << ms_full << " ms, reduced " << ms_lod << " ms; channels "
                                                        << many_full.sys->stats().channels_sampled << " -> "
                                                        << many_lod.sys->stats().channels_sampled);
    CHECK(many_lod.sys->stats().channels_sampled < many_full.sys->stats().channels_sampled);
}
