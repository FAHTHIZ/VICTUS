// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/core/jobs.hpp"

#include <algorithm>
#include <atomic>
#include <numeric>
#include <vector>

using namespace victus::jobs;

TEST_CASE("jobs: submit and wait on counter") {
    JobSystem js(4);
    CHECK(js.worker_count() == 4);
    std::atomic<int> sum{0};
    Counter c;
    for (int i = 1; i <= 1000; ++i) js.submit([&sum, i] { sum += i; }, &c);
    js.wait(c);
    CHECK(c.done());
    CHECK(sum == 500500);
}

TEST_CASE("jobs: parallel_for covers every index exactly once") {
    JobSystem js(3);
    std::vector<int> hits(100000, 0);
    js.parallel_for(0, hits.size(), 64, [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i) hits[i] += 1;
    });
    CHECK(std::accumulate(hits.begin(), hits.end(), 0) == 100000);
    CHECK(*std::min_element(hits.begin(), hits.end()) == 1);
}

TEST_CASE("jobs: nested waits do not deadlock") {
    JobSystem js(2);
    std::atomic<int> leaf{0};
    Counter outer;
    for (int i = 0; i < 16; ++i) {
        js.submit(
            [&] {
                Counter inner;
                for (int j = 0; j < 16; ++j) js.submit([&] { ++leaf; }, &inner);
                js.wait(inner);
            },
            &outer);
    }
    js.wait(outer);
    CHECK(leaf == 256);
}

TEST_CASE("jobs: priorities all execute") {
    JobSystem js(1);
    std::atomic<int> n{0};
    Counter c;
    js.submit([&] { ++n; }, &c, Priority::Background);
    js.submit([&] { ++n; }, &c, Priority::High);
    js.submit([&] { ++n; }, &c, Priority::Normal);
    js.wait(c);
    CHECK(n == 3);
}

TEST_CASE("task graph: dependencies are respected") {
    JobSystem js(4);
    std::atomic<int> order{0};
    int a_at = -1, b_at = -1, c_at = -1, d_at = -1;
    TaskGraph g;
    const auto a = g.add("a", [&] { a_at = order++; });
    const auto b = g.add("b", [&] { b_at = order++; });
    const auto c = g.add("c", [&] { c_at = order++; });
    const auto d = g.add("d", [&] { d_at = order++; });
    g.precede(a, b);
    g.precede(a, c);
    g.precede(b, d);
    g.precede(c, d);
    for (int run = 0; run < 50; ++run) {
        order = 0;
        REQUIRE(g.run(js));
        CHECK(a_at == 0);
        CHECK(b_at > a_at);
        CHECK(c_at > a_at);
        CHECK(d_at == 3);
    }
}

TEST_CASE("task graph: cycles are rejected") {
    JobSystem js(1);
    TaskGraph g;
    const auto a = g.add("a", [] {});
    const auto b = g.add("b", [] {});
    g.precede(a, b);
    g.precede(b, a);
    CHECK(g.has_cycle());
    CHECK_FALSE(g.run(js));
}
