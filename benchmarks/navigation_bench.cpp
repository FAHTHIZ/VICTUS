// SPDX-License-Identifier: Apache-2.0
// Navigation benchmarks: navmesh build from static colliders (a 60 x 60 m floor with a grid of
// pillar/wall obstacles), straight-path queries across the level, and one crowd update for N
// agents walking to random destinations with local avoidance.
#include "bench_harness.hpp"

#include "victus/navigation/navigation.hpp"
#include "victus/physics/physics.hpp"

#include <cstdlib>
#include <string>
#include <vector>

using namespace victus;

namespace {

void level(scene::Scene& s) {
    const scene::Entity floor = s.create("Floor");
    s.world().get<scene::Transform>(floor)->position = {0, -0.5f, 0};
    s.world().add<physics::ColliderComponent>(floor).size = {60, 1, 60};
    // 10 x 10 obstacles: alternating pillars (cylinders) and short walls.
    for (int i = 0; i < 10; ++i) {
        for (int j = 0; j < 10; ++j) {
            const scene::Entity e = s.create("Obstacle");
            s.world().get<scene::Transform>(e)->position = {-27.0f + 6.0f * static_cast<float>(i), 1, -27.0f + 6.0f * static_cast<float>(j)};
            auto& c = s.world().add<physics::ColliderComponent>(e);
            if ((i + j) % 2) {
                c.shape = physics::ColliderComponent::Shape::Cylinder;
                c.radius = 0.8f;
                c.height = 2;
            } else {
                c.size = {3, 2, 0.6f};
            }
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    bench::Harness h(bench::parse_options(argc, argv));
    navigation::register_components();
    scene::Scene s;
    level(s);
    navigation::NavigationSystem nav;
    h.run("nav/build_60m_100_obstacles", 1, [&] {
        if (!nav.build(s).ok()) std::abort();
    });
    if (!nav.valid()) return 2;
    std::vector<math::Vec3> path;
    std::uint32_t seed = 1;
    h.run("nav/find_path_across_level", 1, [&] {
        const auto a = nav.random_point(seed++), b = nav.random_point(seed++);
        if (a && b) nav.find_path(*a, *b, path);
        bench::do_not_optimize(path);
    });
    for (const int count : {16, 128}) {
        scene::Scene cs;
        level(cs);
        navigation::NavigationSystem crowd;
        gameplay::Simulation sim;
        navigation::add_navigation_systems(sim, crowd);
        std::vector<scene::Entity> agents;
        for (int i = 0; i < count; ++i) {
            const scene::Entity e = cs.create("Agent");
            cs.world().add<navigation::NavAgentComponent>(e);
            agents.push_back(e);
        }
        if (!crowd.build(cs).ok()) return 2;
        for (int i = 0; i < count; ++i) {
            if (const auto p = crowd.random_point(1000u + static_cast<std::uint32_t>(i)))
                cs.world().get<scene::Transform>(agents[static_cast<std::size_t>(i)])->position = *p;
        }
        crowd.update(cs, 1.0f / 60.0f);
        std::uint32_t goal = 5000;
        auto retarget = [&] {
            for (const scene::Entity e : agents) {
                if (const auto p = crowd.random_point(goal++)) crowd.set_destination(cs, e, *p);
            }
        };
        retarget();
        int frame = 0;
        h.run("nav/crowd_update_" + std::to_string(count) + "_agents", static_cast<std::uint64_t>(count), [&] {
            if (++frame % 600 == 0) retarget(); // keep them walking
            crowd.update(cs, 1.0f / 60.0f);
        });
        if (crowd.stats().agents != static_cast<std::uint32_t>(count)) return 3;
    }
    return 0;
}
