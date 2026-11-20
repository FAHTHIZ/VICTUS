// SPDX-License-Identifier: Apache-2.0
#include "victus/player/bake_lighting.hpp"

#include "victus/animation/animation.hpp"
#include "victus/assets/database.hpp"
#include "victus/core/fs.hpp"
#include "victus/core/log.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/navigation/navigation.hpp"
#include "victus/physics/physics.hpp"
#include "victus/project/project.hpp"
#include "victus/script/behaviours.hpp"
#include "victus/script/script.hpp"
#include "victus/sequencer/sequencer.hpp"

#include <chrono>
#include <filesystem>

namespace victus::player {

namespace {

// Something on this entity moves it (or plays it) at run time.
bool moves(const ecs::World& w, scene::Entity e) {
    if (const auto* rb = w.get<physics::RigidBodyComponent>(e); rb && rb->type != physics::BodyType::Static) return true;
    return w.has<physics::CharacterControllerComponent>(e) || w.has<navigation::NavAgentComponent>(e) ||
           w.has<animation::AnimatorComponent>(e) || w.has<script::ScriptComponent>(e) || w.has<gameplay::RotatorComponent>(e) ||
           w.has<gameplay::OscillatorComponent>(e) || w.has<script::MovingPlatformComponent>(e) ||
           w.has<sequencer::SequencePlayerComponent>(e);
}

} // namespace

bool is_static_for_lighting(scene::Scene& scene, scene::Entity entity) {
    const ecs::World& w = scene.world();
    // A moving parent (a character's held prop, a vehicle's wheel, a turret's head) moves the child too.
    for (scene::Entity e = entity; !e.is_null(); e = scene.parent(e))
        if (moves(w, e)) return false;
    return true;
}

std::uint32_t mark_static_lighting(scene::Scene& scene) {
    std::vector<scene::Entity> candidates;
    scene.world().each<scene::MeshRendererComponent>([&](ecs::Entity e, scene::MeshRendererComponent& mr) {
        if (!mr.lightmapped && mr.visible) candidates.push_back(e);
    });
    std::uint32_t marked = 0;
    for (scene::Entity e : candidates) {
        if (!is_static_for_lighting(scene, e)) continue;
        scene.world().get<scene::MeshRendererComponent>(e)->lightmapped = true;
        ++marked;
    }
    return marked;
}

Result<BakeLightingReport> bake_project_lighting(const project::Project& project, const BakeLightingOptions& options) {
    BakeLightingReport report;
    report.scene = options.scene.empty() ? project.settings().startup_scene : options.scene;
    auto path = project.resolve(report.scene);
    if (!path) return std::move(path).error();
    scene::Scene s;
    if (auto st = s.load_file(*path); !st) return std::move(st).error();
    auto db = assets::AssetDatabase::open(project.root());
    if (!db) return std::move(db).error();
    const auto imported = (*db)->import_all();
    for (const auto& m : imported.messages) VICTUS_LOG_WARN("bake", "{}", m);
    if (options.mark_static) report.marked = mark_static_lighting(s);
    s.update_transforms();
    const auto t0 = std::chrono::steady_clock::now();
    auto bake = runtime::bake_scene_lightmap(s, db->get(), options.settings, options.jobs);
    if (!bake) return std::move(bake).error();
    report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    report.bake = std::move(*bake);

    // Where the bake goes: `out`, else the Environment's current lightmap, else next to the scene.
    ecs::World& w = s.world();
    gameplay::EnvironmentComponent* env = nullptr;
    w.each<gameplay::EnvironmentComponent>([&](gameplay::EnvironmentComponent& e) {
        if (!env) env = &e;
    });
    report.out = options.out;
    if (report.out.empty()) report.out = env && !env->lightmap.empty() ? env->lightmap : runtime::default_lightmap_path(report.scene);
    if (options.dry_run) return report;
    auto target = project.resolve(report.out);
    if (!target) return std::move(target).error();
    std::error_code ec;
    std::filesystem::create_directories(target->parent_path(), ec);
    if (auto wr = fs::write_atomic(*target, render::encode_lightmap_file(report.bake.file)); !wr) return std::move(wr).error();
    bool rewrite = report.marked > 0;
    if (!env) {
        env = &w.add<gameplay::EnvironmentComponent>(s.create("Environment"));
        report.added_environment = true;
        rewrite = true;
    }
    if (env->lightmap != report.out) {
        env->lightmap = report.out;
        rewrite = true;
    }
    if (rewrite)
        if (auto sv = s.save_file(*path); !sv) return std::move(sv).error();
    report.scene_updated = rewrite;
    return report;
}

Result<bool> bake_template_lighting(const project::Project& project, jobs::JobSystem* jobs) {
    auto path = project.startup_scene_path();
    if (!path) return std::move(path).error();
    scene::Scene s;
    if (auto st = s.load_file(*path); !st) return std::move(st).error();
    bool any = false;
    s.world().each<scene::MeshRendererComponent>([&](scene::MeshRendererComponent& mr) { any = any || (mr.lightmapped && mr.visible); });
    if (!any) return false;
    BakeLightingOptions o;
    o.jobs = jobs;
    auto r = bake_project_lighting(project, o);
    if (!r) return std::move(r).error();
    for (const auto& warning : r->bake.warnings) VICTUS_LOG_WARN("bake", "{}", warning);
    VICTUS_LOG_INFO("bake", "baked template lighting for {} renderers and {} light probes into {} in {:.1f} s", r->bake.receivers,
                    r->bake.stats.probes, r->out, r->seconds);
    return true;
}

} // namespace victus::player
