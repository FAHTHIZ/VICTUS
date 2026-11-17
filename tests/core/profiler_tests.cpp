// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/core/fs.hpp"
#include "victus/core/json.hpp"
#include "victus/core/profiler.hpp"

#include <chrono>
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

namespace {
void busy_ms(double ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(static_cast<long long>(ms * 1000));
    while (std::chrono::steady_clock::now() < until) {
    }
}
} // namespace

TEST_CASE("profiler: intern returns stable pointers for equal names") {
    const char* a = profiler::intern(std::string("system.") + "physics");
    const char* b = profiler::intern("system.physics");
    CHECK(a == b);
    CHECK(std::string(a) == "system.physics");
    CHECK(profiler::intern("system.other") != a);
}

TEST_CASE("profiler: frame profiler aggregates frame times and per-scope totals") {
    profiler::FrameProfiler fp(100);
    CHECK(profiler::capturing());
    fp.frame(); // first boundary: no frame yet
    CHECK(fp.frames_profiled() == 0);
    for (int i = 0; i < 6; ++i) {
        {
            VICTUS_PROFILE_SCOPE("work");
            busy_ms(2.0);
            { VICTUS_PROFILE_SCOPE("work"); } // nested same name: counted once
        }
        for (int k = 0; k < 3; ++k) {
            VICTUS_PROFILE_SCOPE("small");
        }
        fp.frame();
    }
    CHECK(fp.frames_profiled() == 6);
    const auto r = fp.report();
    CHECK(r.frames == 6);
    CHECK(r.p50_ms >= 2.0);
    CHECK(r.p95_ms >= r.p50_ms);
    CHECK(r.p99_ms >= r.p95_ms);
    CHECK(r.max_ms >= r.p99_ms);
    REQUIRE(r.scopes.size() == 2);
    CHECK(r.scopes[0].name == "work"); // sorted by time
    CHECK(r.scopes[0].mean_ms >= 2.0);
    CHECK(r.scopes[0].mean_ms < r.mean_ms + 0.5);
    CHECK(r.scopes[0].calls_per_frame == doctest::Approx(2.0));
    CHECK(r.scopes[1].name == "small");
    CHECK(r.scopes[1].calls_per_frame == doctest::Approx(3.0));

    fp.set_paused(true);
    { VICTUS_PROFILE_SCOPE("work"); }
    fp.frame();
    CHECK(fp.frames_profiled() == 6);
    fp.set_paused(false);
    fp.reset();
    CHECK(fp.report().frames == 0);
    CHECK(fp.report().scopes.empty());
}

TEST_CASE("profiler: frame profiler records the zones of the next frames for a trace") {
    profiler::FrameProfiler fp(10);
    fp.frame();
    fp.record(2);
    CHECK(fp.recording());
    CHECK(fp.take_recording().empty());
    { VICTUS_PROFILE_SCOPE("a"); }
    fp.frame();
    { VICTUS_PROFILE_SCOPE("b"); }
    fp.frame();
    { VICTUS_PROFILE_SCOPE("c"); } // after the recording window
    fp.frame();
    CHECK_FALSE(fp.recording());
    auto zones = fp.take_recording();
    REQUIRE(zones.size() == 2);
    CHECK(std::string(zones[0].name) == "a");
    CHECK(std::string(zones[1].name) == "b");
    CHECK(fp.take_recording().empty());

    fp.record(100);
    { VICTUS_PROFILE_SCOPE("d"); }
    fp.frame();
    fp.stop_recording();
    zones = fp.take_recording();
    REQUIRE(zones.size() == 1);
    CHECK(std::string(zones[0].name) == "d");
}

TEST_CASE("profiler: report json carries frame percentiles, scopes and counters") {
    profiler::FrameReport r;
    r.frames = 3;
    r.mean_ms = 16.5;
    r.p50_ms = 16.0;
    r.p95_ms = 18.0;
    r.p99_ms = 19.0;
    r.max_ms = 20.0;
    r.scopes.push_back({"pass.forward", 4.0, 5.0, 6.0, 4.0, 1.0});
    r.counters.emplace_back("gpu.texture_bytes", 1024.0);
    auto parsed = json::parse(profiler::report_json(r));
    REQUIRE(parsed);
    const auto& v = *parsed;
    CHECK(v["frames"].as_number() == 3);
    CHECK(v["frame"]["p95_ms"].as_number() == doctest::Approx(18.0));
    CHECK(v["frame"]["p99_ms"].as_number() == doctest::Approx(19.0));
    CHECK(v["scopes"][0]["name"].as_string() == "pass.forward");
    CHECK(v["scopes"][0]["mean_ms"].as_number() == doctest::Approx(4.0));
    CHECK(v["counters"]["gpu.texture_bytes"].as_number() == 1024);
}

TEST_CASE("profiler: scopes first seen late are averaged over the whole window") {
    profiler::FrameProfiler fp(50);
    fp.frame();
    for (int i = 0; i < 3; ++i) fp.frame();
    { VICTUS_PROFILE_SCOPE("late"); }
    fp.frame();
    const auto r = fp.report();
    REQUIRE(r.frames == 4);
    REQUIRE(r.scopes.size() == 1);
    CHECK(r.scopes[0].calls_per_frame == doctest::Approx(0.25));
}
