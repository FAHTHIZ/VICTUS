// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/core/fs.hpp"
#include "victus/core/json.hpp"
#include "victus/core/profiler.hpp"

#include <filesystem>
#include <thread>

using namespace victus;

TEST_CASE("profiler: zones are captured only while capturing, nested depth recorded") {
    { VICTUS_PROFILE_SCOPE("before"); }
    profiler::begin_capture();
    {
        VICTUS_PROFILE_SCOPE("outer");
        { VICTUS_PROFILE_SCOPE("inner"); }
    }
    std::thread t([] {
        profiler::set_thread_name("test-worker");
        VICTUS_PROFILE_SCOPE("thread");
    });
    t.join();
    const auto zones = profiler::end_capture();
    REQUIRE(zones.size() == 3);
    CHECK(std::string(zones[0].name) == "outer");
    CHECK(zones[0].depth == 0);
    CHECK(std::string(zones[1].name) == "inner");
    CHECK(zones[1].depth == 1);
    CHECK(zones[1].begin_ns >= zones[0].begin_ns);
    CHECK(zones[1].end_ns <= zones[0].end_ns);
    CHECK(zones[2].thread != zones[0].thread);

    const auto path = std::filesystem::temp_directory_path() / "victus_trace_test.json";
    REQUIRE(profiler::write_chrome_trace(path, zones).ok());
    auto doc = json::parse(*fs::read_text(path));
    REQUIRE(doc.ok());
    CHECK((*doc)["traceEvents"].size() >= 3);
    std::filesystem::remove(path);
}

TEST_CASE("sample window: percentiles over rolling window") {
    profiler::SampleWindow w(100);
    for (int i = 1; i <= 100; ++i) w.add(i);
    CHECK(w.percentile(0) == 1);
    CHECK(w.percentile(100) == 100);
    CHECK(w.percentile(50) == doctest::Approx(50.5));
    CHECK(w.mean() == doctest::Approx(50.5));
    w.add(1000);
    CHECK(w.count() == 100);
    CHECK(w.max() == 1000);
    CHECK(w.min() == 2);
    CHECK(w.ordered().back() == 1000);
}
