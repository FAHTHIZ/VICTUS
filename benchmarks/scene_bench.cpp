// SPDX-License-Identifier: Apache-2.0
// ECS and scene benchmarks: structural changes, query iteration (serial and parallel),
// hierarchy transform propagation and scene (de)serialization.
#include "bench_harness.hpp"

#include "victus/core/jobs.hpp"
#include "victus/scene/scene.hpp"

#include <vector>

using namespace victus;

namespace {
struct Position {
    math::Vec3 v;
};
struct Velocity {
    math::Vec3 v{0.1f, 0.2f, 0.3f};
};
struct Health {
    float hp{100};
};
} // namespace

int main(int argc, char** argv) {
    bench::Harness h(bench::parse_options(argc, argv));
    constexpr std::size_t kEntities = 100'000;

    h.run("ecs/create_add2_destroy_100k", kEntities, [&] {
        ecs::World w;
        std::vector<ecs::Entity> es(kEntities);
        for (auto& e : es) {
            e = w.create();
            w.add<Position>(e);
            w.add<Velocity>(e);
        }
        for (const auto e : es) w.destroy(e);
    });

    {
        ecs::World w;
        for (std::size_t i = 0; i < 1'000'000; ++i) {
            const auto e = w.create();
            w.add<Position>(e);
            w.add<Velocity>(e);
            if (i % 3 == 0) w.add<Health>(e); // two archetypes
        }
        h.run("ecs/each_integrate_1M", 1'000'000, [&] {
            w.each<Position, Velocity>([](Position& p, const Velocity& v) { p.v += v.v * (1.0f / 60.0f); });
        });
        jobs::JobSystem js;
        h.run("ecs/par_each_integrate_1M", 1'000'000, [&] {
            w.par_each<Position, Velocity>(js, 8192, [](Position& p, const Velocity& v) { p.v += v.v * (1.0f / 60.0f); });
        });
        ecs::Entity probe = w.entity_at(500'000);
        h.run("ecs/random_get_1M", 1'000'000, [&] {
            float sum = 0;
            for (std::uint32_t i = 0; i < 1'000'000; i += 1) {
                const auto e = w.entity_at((i * 2654435761u) % 1'000'000u);
                if (const auto* p = w.get<Position>(e)) sum += p->v.x;
            }
            bench::do_not_optimize(sum);
        });
        bench::do_not_optimize(probe);
    }

    {
        // 10k-entity scene: 1000 roots each with 9 children (two levels).
        scene::Scene s;
        for (int r = 0; r < 1000; ++r) {
            const auto root = s.create("root");
            s.world().get<scene::Transform>(root)->position = {static_cast<float>(r), 0, 0};
            auto parent = root;
            for (int c = 0; c < 9; ++c) {
                const auto child = s.create("child", c % 3 == 0 ? root : parent);
                s.world().get<scene::Transform>(child)->position = {0, 1, 0};
                s.world().add<scene::MeshRendererComponent>(child).primitive = "cube";
                parent = child;
            }
        }
        s.update_transforms();
        // Static scene: nothing moved since the previous update.
        h.run("scene/update_transforms_10k", s.size(), [&] { s.update_transforms(); });
        // Every root moves each frame, so every world matrix is recomputed.
        const std::vector<scene::Entity> roots = s.roots();
        float phase = 0;
        h.run("scene/update_transforms_10k_all_moving", s.size(), [&] {
            phase += 0.01f;
            for (const auto r : roots) s.world().get<scene::Transform>(r)->position.y = phase;
            s.update_transforms();
        });
        // Nothing moves, but a component is added or removed every frame (storage relocates).
        bool tagged = false;
        h.run("scene/update_transforms_10k_component_churn", s.size(), [&] {
            if (tagged) s.world().remove<scene::CameraComponent>(roots[0]);
            else s.world().add<scene::CameraComponent>(roots[0]);
            tagged = !tagged;
            s.update_transforms();
        });
        json::Value doc;
        h.run("scene/save_10k", s.size(), [&] { doc = s.save(); });
        const std::string text = json::stringify(doc);
        h.run("scene/stringify_10k", s.size(), [&] { bench::do_not_optimize(json::stringify(doc)); });
        h.run("scene/parse_load_10k", s.size(), [&] {
            auto parsed = json::parse(text);
            scene::Scene t;
            auto st = t.load(*parsed);
            bench::do_not_optimize(st.ok());
        });
    }
    return h.finish();
}
