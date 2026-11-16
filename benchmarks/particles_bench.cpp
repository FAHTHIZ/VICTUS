// SPDX-License-Identifier: Apache-2.0
// Particle benchmarks: one ParticleSystem::update for N emitters at their 256-particle cap in
// steady state (particles die and respawn every frame), with gravity, drag and spin, including
// billboard generation. Items are live particles. "_1thread" runs serially, "_jobs" through
// parallel_for (one emitter per task). "particles/gather" measures collecting every emitter's
// billboards for rendering.
#include "bench_harness.hpp"

#include "victus/core/jobs.hpp"
#include "victus/particles/particles.hpp"

#include <string>

using namespace victus;

namespace {

void build(scene::Scene& s, int emitters) {
    for (int i = 0; i < emitters; ++i) {
        const scene::Entity e = s.create("Fx " + std::to_string(i));
        s.world().get<scene::Transform>(e)->position = {static_cast<float>(i % 8) * 2.0f, 0, static_cast<float>(i / 8) * 2.0f};
        auto& c = s.world().add<particles::ParticleEmitterComponent>(e);
        c.seed = static_cast<std::uint32_t>(i + 1);
        c.max_particles = 256;
        c.rate = 400;      // above lifetime * cap: the emitter stays full
        c.lifetime_min = 0.5f;
        c.lifetime_max = 0.7f;
        c.shape = particles::Shape::Cone;
        c.gravity = {0, -2, 0};
        c.drag = 0.5f;
        c.spin_min = -90;
        c.spin_max = 90;
        c.size_variation = 0.3f;
    }
}

} // namespace

int main(int argc, char** argv) {
    bench::Harness h(bench::parse_options(argc, argv));
    jobs::JobSystem workers(0);
    for (const bool threaded : {false, true}) {
        const std::string suffix = threaded ? "_jobs" : "_1thread";
        for (const int n : {16, 64}) {
            scene::Scene s;
            build(s, n);
            particles::ParticleSystem sys({.jobs = threaded ? &workers : nullptr, .parallel_threshold = 2});
            for (int f = 0; f < 120; ++f) sys.update(s, 1.0f / 60); // reach steady state
            const std::uint32_t alive = sys.stats().alive;
            if (alive < static_cast<std::uint32_t>(n) * 240) return 2;
            h.run("particles/update_" + std::to_string(n) + "x256" + suffix, alive, [&] { sys.update(s, 1.0f / 60); });
            if (!threaded && n == 64) {
                std::vector<particles::EmitterView> views;
                views.reserve(static_cast<std::size_t>(n));
                h.run("particles/gather_64x256", alive, [&] {
                    views.clear();
                    particles::gather(s, views);
                });
            }
        }
    }
    return h.finish();
}
