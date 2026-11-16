// SPDX-License-Identifier: Apache-2.0
#include "bench_harness.hpp"

#include "victus/core/hash.hpp"
#include "victus/core/jobs.hpp"
#include "victus/core/json.hpp"
#include "victus/core/math.hpp"
#include "victus/core/memory.hpp"

#include <atomic>
#include <cstdlib>
#include <vector>

using namespace victus;

int main(int argc, char** argv) {
    bench::Harness h(bench::parse_options(argc, argv));

    {
        jobs::JobSystem js;
        constexpr int kJobs = 100000;
        h.run("jobs/submit_wait_100k_empty", kJobs, [&] {
            jobs::Counter c;
            for (int i = 0; i < kJobs; ++i) js.submit([] {}, &c);
            js.wait(c);
        });
        std::vector<math::Vec3> pos(1'000'000, math::Vec3{1, 2, 3}), vel(1'000'000, math::Vec3{0.1f, 0.2f, 0.3f});
        h.run("jobs/parallel_for_integrate_1M", pos.size(), [&] {
            js.parallel_for(0, pos.size(), 4096, [&](std::size_t b, std::size_t e) {
                for (std::size_t i = b; i < e; ++i) pos[i] += vel[i] * (1.0f / 60.0f);
            });
        });
        h.run("serial/integrate_1M", pos.size(), [&] {
            for (std::size_t i = 0; i < pos.size(); ++i) pos[i] += vel[i] * (1.0f / 60.0f);
        });
    }

    {
        std::vector<math::Mat4> out(100000);
        h.run("math/trs_compose_100k", out.size(), [&] {
            for (std::size_t i = 0; i < out.size(); ++i)
                out[i] = math::Mat4::trs({float(i), 0, 0}, math::Quat::euler(0.1f, 0.2f, 0.3f), {1, 1, 1});
        });
    }

    {
        memory::LinearArena arena(1 << 20);
        constexpr int kAllocs = 100000;
        h.run("memory/arena_alloc_100k", kAllocs, [&] {
            for (int i = 0; i < kAllocs; ++i) (void)arena.allocate(32, 16);
            arena.reset();
        });
        std::vector<void*> ptrs(kAllocs);
        h.run("memory/malloc_free_100k", kAllocs, [&] {
            for (int i = 0; i < kAllocs; ++i) ptrs[static_cast<std::size_t>(i)] = std::malloc(32);
            for (void* p : ptrs) std::free(p);
        });
    }

    {
        json::Value doc = json::Value::array();
        Random rng(1);
        for (int i = 0; i < 20000; ++i) {
            json::Value e = json::Value::object();
            e.set("id", Uuid::generate().to_string());
            e.set("name", "entity_" + std::to_string(i));
            json::Value p = json::Value::array();
            for (int k = 0; k < 3; ++k) p.push(rng.next_float());
            e.set("position", p);
            doc.push(e);
        }
        const std::string text = json::stringify(doc, false);
        h.run("json/parse_20k_entities", text.size(), [&] { (void)json::parse(text); });
        h.run("json/stringify_20k_entities", text.size(), [&] { (void)json::stringify(doc, false); });
    }
    return h.finish();
}
