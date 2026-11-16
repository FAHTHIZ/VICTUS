// SPDX-License-Identifier: Apache-2.0
// Physics benchmarks: one fixed 1/60 s step (sync + simulate + transform write-back) for piles of
// dynamic bodies resting on a static ground, in 3D (Jolt) and 2D (Box2D). The piles are settled
// before measuring. "_sleeping" measures a settled pile (bodies asleep: sync + broad phase only);
// "_awake" applies a tiny vertical impulse to every body before each step so the full solver runs
// on every contact (the impulse calls are included in the time).
#include "bench_harness.hpp"

#include "victus/core/jobs.hpp"
#include "victus/physics/physics.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace victus;

namespace {

void build_3d(scene::Scene& s, int side) {
    const auto ground = s.create("ground");
    s.world().add<physics::RigidBodyComponent>(ground).type = physics::BodyType::Static;
    s.world().add<physics::ColliderComponent>(ground).size = {400, 1, 400};
    s.world().get<scene::Transform>(ground)->position = {0, -0.5f, 0};
    for (int y = 0; y < 4; ++y)
        for (int z = 0; z < side; ++z)
            for (int x = 0; x < side; ++x) {
                const auto e = s.create("box");
                s.world().get<scene::Transform>(e)->position = {static_cast<float>(x) * 1.1f, 0.5f + static_cast<float>(y) * 1.05f,
                                                                static_cast<float>(z) * 1.1f};
                s.world().add<physics::RigidBodyComponent>(e);
                s.world().add<physics::ColliderComponent>(e);
            }
}

void build_2d(scene::Scene& s, int columns) {
    const auto ground = s.create("ground");
    s.world().add<physics::RigidBody2DComponent>(ground).type = physics::BodyType::Static;
    s.world().add<physics::Collider2DComponent>(ground).size = {4000, 1};
    s.world().get<scene::Transform>(ground)->position = {0, -0.5f, 0};
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < columns; ++x) {
            const auto e = s.create("box");
            s.world().get<scene::Transform>(e)->position = {static_cast<float>(x) * 1.1f, 0.5f + static_cast<float>(y) * 1.02f, 0};
            s.world().add<physics::RigidBody2DComponent>(e);
            s.world().add<physics::Collider2DComponent>(e);
        }
}

template <class C>
std::vector<scene::Entity> entities_with(scene::Scene& s) {
    std::vector<scene::Entity> out;
    s.world().each<C>([&](scene::Entity e, C& rb) {
        if (rb.type == physics::BodyType::Dynamic) out.push_back(e);
    });
    return out;
}

} // namespace

int main(int argc, char** argv) {
    bench::Harness h(bench::parse_options(argc, argv));
    physics::register_components();
    jobs::JobSystem workers(0);
    constexpr float kDt = 1.0f / 60.0f;
    for (const bool threaded : {false, true}) {
        const std::string suffix = threaded ? "_jobs" : "_1thread";
        for (const int side : {16, 32}) { // 4 layers: 1 024 / 4 096 bodies
            scene::Scene s;
            build_3d(s, side);
            physics::PhysicsSettings settings;
            if (threaded) settings.jobs = &workers;
            auto w = physics::PhysicsWorld::create(settings);
            if (!w) return 1;
            for (int i = 0; i < 120; ++i) (*w)->step(s, kDt);
            const int n = side * side * 4;
            const auto bodies = entities_with<physics::RigidBodyComponent>(s);
            h.run("physics3d/step_" + std::to_string(n) + "_boxes_sleeping" + suffix, static_cast<std::size_t>(n),
                  [&] { (*w)->step(s, kDt); });
            h.run("physics3d/step_" + std::to_string(n) + "_boxes_awake" + suffix, static_cast<std::size_t>(n), [&] {
                for (const auto e : bodies) (*w)->add_impulse(e, {0, 1e-3f, 0});
                (*w)->step(s, kDt);
            });
            if ((*w)->stats().active_3d < static_cast<std::uint32_t>(n) / 2) return 2; // awake case must be awake
        }
        for (const int columns : {100, 400}) { // 10 rows: 1 000 / 4 000 bodies
            scene::Scene s;
            build_2d(s, columns);
            physics::PhysicsSettings settings;
            if (threaded) settings.jobs = &workers;
            auto w = physics::PhysicsWorld::create(settings);
            if (!w) return 1;
            for (int i = 0; i < 120; ++i) (*w)->step(s, kDt);
            const int n = columns * 10;
            const auto bodies = entities_with<physics::RigidBody2DComponent>(s);
            h.run("physics2d/step_" + std::to_string(n) + "_boxes_sleeping" + suffix, static_cast<std::size_t>(n),
                  [&] { (*w)->step(s, kDt); });
            h.run("physics2d/step_" + std::to_string(n) + "_boxes_awake" + suffix, static_cast<std::size_t>(n), [&] {
                for (const auto e : bodies) (*w)->add_impulse(e, {0, 1e-3f, 0});
                (*w)->step(s, kDt);
            });
            if ((*w)->stats().awake_2d < static_cast<std::uint32_t>(n) / 2) return 2;
        }
    }
    {
        scene::Scene s;
        build_3d(s, 32);
        auto w = physics::PhysicsWorld::create();
        if (!w) return 1;
        (*w)->sync(s, kDt);
        float x = 0;
        h.run("physics3d/raycast_down_4096_boxes", 1000, [&] {
            for (int i = 0; i < 1000; ++i) {
                x = static_cast<float>(i % 32) * 1.1f;
                auto hit = (*w)->raycast({x, 20, 5.5f}, {0, -1, 0}, 50);
                bench::do_not_optimize(hit);
            }
        });
    }
    return 0;
}
