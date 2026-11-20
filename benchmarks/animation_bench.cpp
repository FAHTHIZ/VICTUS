// SPDX-License-Identifier: Apache-2.0
// Animation benchmarks: one AnimationSystem::update for N animated characters sharing a
// 32-joint skeleton (a branching chain). Every joint has a 31-key linear rotation channel and the
// root a translation channel, so each update samples 33 channels and writes 32 transforms per
// character. "_crossfade" measures the same with every character mid-way through a cross-fade
// (two clips sampled and blended). "_1thread" runs serially, "_jobs" through parallel_for.
// "animation/character_*" runs the real Orion starter character (21 joints, 7 clips) under its
// shared locomotion controller, walking-to-running blend (two clips sampled per character).
#include "bench_harness.hpp"

#include "victus/animation/animation.hpp"
#include "victus/assets/cooked.hpp"
#include "victus/assets/importers.hpp"
#include "victus/core/jobs.hpp"
#include "victus/project/characters.hpp"

#include <cmath>
#include <memory>
#include <string>

using namespace victus;
using math::Quat;

namespace {

constexpr int kJoints = 32;
constexpr int kKeys = 31;

animation::Clip make_clip(const char* name, float phase) {
    animation::Clip clip{.name = name, .duration = 2.0f};
    for (int j = 0; j < kJoints; ++j) {
        animation::Channel ch{.node = static_cast<std::uint32_t>(j), .path = animation::Path::Rotation,
                              .interpolation = animation::Interpolation::Linear};
        for (int k = 0; k < kKeys; ++k) {
            const float t = 2.0f * static_cast<float>(k) / static_cast<float>(kKeys - 1);
            ch.times.push_back(t);
            const Quat q = Quat::axis_angle({j % 3 == 0 ? 1.0f : 0.0f, j % 3 == 1 ? 1.0f : 0.0f, j % 3 == 2 ? 1.0f : 0.0f},
                                            0.4f * std::sin(3.14159f * t + phase + static_cast<float>(j)));
            ch.values.insert(ch.values.end(), {q.x, q.y, q.z, q.w});
        }
        clip.channels.push_back(std::move(ch));
    }
    animation::Channel root{.node = 0, .path = animation::Path::Translation, .interpolation = animation::Interpolation::Linear};
    for (int k = 0; k < kKeys; ++k) {
        const float t = 2.0f * static_cast<float>(k) / static_cast<float>(kKeys - 1);
        root.times.push_back(t);
        root.values.insert(root.values.end(), {0, 1.0f + 0.05f * std::sin(6.28318f * t + phase), 0});
    }
    clip.channels.push_back(std::move(root));
    return clip;
}

std::shared_ptr<animation::AnimationSet> make_set() {
    auto set = std::make_shared<animation::AnimationSet>();
    for (int j = 0; j < kJoints; ++j) {
        // Joint j hangs off joint (j-1)/2: a balanced binary tree like limbs off a spine.
        set->nodes.push_back({.name = "Joint" + std::to_string(j), .parent = j == 0 ? -1 : (j - 1) / 2,
                              .rest = {.translation = {0, 0.2f, 0}}});
    }
    set->clips.push_back(make_clip("Walk", 0.0f));
    set->clips.push_back(make_clip("Run", 1.0f));
    return set;
}

void build(scene::Scene& s, const Uuid& model, int n) {
    for (int i = 0; i < n; ++i) {
        const auto root = s.create("Character" + std::to_string(i));
        std::vector<scene::Entity> joints;
        for (int j = 0; j < kJoints; ++j)
            joints.push_back(s.create("Joint" + std::to_string(j), j == 0 ? root : joints[static_cast<std::size_t>((j - 1) / 2)]));
        auto& a = s.world().add<animation::AnimatorComponent>(root);
        a.model = model;
        a.clip = "Walk";
        a.time = 0.01f * static_cast<float>(i);
    }
}

} // namespace

int main(int argc, char** argv) {
    bench::Harness h(bench::parse_options(argc, argv));
    jobs::JobSystem workers(0);
    const Uuid model = Uuid::generate();
    const auto set = make_set();
    for (const bool threaded : {false, true}) {
        const std::string suffix = threaded ? "_jobs" : "_1thread";
        for (const int n : {256, 1024}) {
            scene::Scene s;
            build(s, model, n);
            animation::AnimationSettings settings;
            if (threaded) settings.jobs = &workers;
            animation::AnimationSystem sys(settings);
            sys.add_set(model, set);
            sys.update(s, 1.0f / 60); // bind
            if (sys.stats().animators != static_cast<std::uint32_t>(n)) return 2;
            h.run("animation/update_" + std::to_string(n) + "x32_joints" + suffix, static_cast<std::size_t>(n),
                  [&] { sys.update(s, 1.0f / 60); });
            s.world().each<animation::AnimatorComponent>([&](scene::Entity e, animation::AnimatorComponent&) {
                sys.play(s, e, "Run", 1000.0f); // a fade that never completes during the run
            });
            sys.update(s, 1.0f / 60);
            h.run("animation/update_" + std::to_string(n) + "x32_joints_crossfade" + suffix, static_cast<std::size_t>(n),
                  [&] { sys.update(s, 1.0f / 60); });
        }
    }
    // The Orion starter character: real skeleton, clips and controller.
    const auto* v01 = project::find_starter_character("orion");
    assets::ModelImportSettings import;
    import.optimize_meshes = false;
    auto model_v01 = assets::import_gltf_memory(project::starter_character_glb(*v01), "v01.glb", import);
    if (!model_v01) return 3;
    const auto character_set = std::make_shared<animation::AnimationSet>(assets::make_animation_set(*model_v01));
    Uuid character_id;
    if (!Uuid::parse(v01->uuid, character_id)) return 3;
    const std::string controller = project::starter_character_controller();
    for (const bool threaded : {false, true}) {
        const std::string suffix = threaded ? "_jobs" : "_1thread";
        for (const int n : {1, 16, 128}) {
            scene::Scene s;
            for (int i = 0; i < n; ++i) {
                auto e = project::instantiate_starter_character(s, *v01, "Robot", scene::kNullEntity);
                if (!e) return 4;
                s.world().get<animation::AnimatorComponent>(*e)->time = 0.01f * static_cast<float>(i);
            }
            animation::AnimationSettings settings;
            if (threaded) settings.jobs = &workers;
            settings.controllers = [&](std::string_view) -> Result<std::string> { return controller; };
            animation::AnimationSystem sys(settings);
            sys.add_set(character_id, character_set);
            sys.update(s, 1.0f / 60); // bind
            s.world().each<animation::AnimatorComponent>([&](scene::Entity e, animation::AnimatorComponent&) {
                (void)sys.set_parameter(s, e, "speed", 3.5f); // between Walk and Run
            });
            sys.update(s, 1.0f / 60);
            if (sys.stats().animators != static_cast<std::uint32_t>(n)) return 5;
            h.run("animation/character_" + std::to_string(n) + "x21_joints_controller" + suffix, static_cast<std::size_t>(n),
                  [&] { sys.update(s, 1.0f / 60); });
        }
    }
    return h.finish();
}
