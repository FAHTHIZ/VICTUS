// SPDX-License-Identifier: Apache-2.0
#include "bench_harness.hpp"

#include "victus/core/build_info.hpp"
#include "victus/core/cli.hpp"
#include "victus/core/fs.hpp"
#include "victus/core/memory.hpp"
#include "victus/core/profiler.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <thread>

namespace victus::bench {

Options parse_options(int argc, char** argv) {
    cli::Parser p(argv[0], "VICTUS benchmark runner");
    p.option("warmup", "warm-up repetitions", "3")
        .option("reps", "measured repetitions", "15")
        .option("json", "write machine-readable results to this path")
        .option("filter", "only run cases containing this text");
    if (auto s = p.parse(argc, argv); !s.ok()) {
        std::fprintf(stderr, "%s\n%s", s.error().message.c_str(), p.help().c_str());
        std::exit(2);
    }
    Options o;
    o.warmup = static_cast<int>(p.get_number("warmup", 3));
    o.repetitions = static_cast<int>(p.get_number("reps", 15));
    o.output_path = p.get("json");
    o.filter = p.get("filter");
    return o;
}

json::Value machine_metadata() {
    json::Value m = json::Value::object();
    m.set("victus_version", build::version());
    m.set("git_revision", build::git_revision());
    m.set("platform", build::platform());
    m.set("architecture", build::architecture());
    m.set("compiler", build::compiler());
    m.set("build_type", build::build_type());
    m.set("hardware_threads", static_cast<std::uint32_t>(std::thread::hardware_concurrency()));
#if defined(__linux__)
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.rfind("model name", 0) == 0) {
            m.set("cpu", line.substr(line.find(':') + 2));
            break;
        }
    }
#endif
    m.set("allocation_tracking", memory::allocation_tracking_enabled());
    m.set("timestamp_unix", static_cast<std::int64_t>(std::time(nullptr)));
    return m;
}

void Harness::run(const std::string& name, std::uint64_t items, const std::function<void()>& fn) {
    if (!options_.filter.empty() && name.find(options_.filter) == std::string::npos) return;
    for (int i = 0; i < options_.warmup; ++i) fn();
    profiler::SampleWindow window(static_cast<std::size_t>(options_.repetitions));
    std::uint64_t allocs = 0;
    for (int i = 0; i < options_.repetitions; ++i) {
        const auto a0 = memory::allocation_stats().allocations;
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        const auto t1 = std::chrono::steady_clock::now();
        allocs += memory::allocation_stats().allocations - a0;
        window.add(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    CaseResult r;
    r.name = name;
    r.items_per_rep = items;
    r.p50_ms = window.percentile(50);
    r.p95_ms = window.percentile(95);
    r.min_ms = window.min();
    r.max_ms = window.max();
    r.mean_ms = window.mean();
    r.items_per_second = r.p50_ms > 0 ? static_cast<double>(items) / (r.p50_ms / 1000.0) : 0;
    r.allocations_per_rep = static_cast<double>(allocs) / options_.repetitions;
    std::printf("%-44s p50 %9.3f ms  p95 %9.3f ms  %12.3e items/s  %8.1f allocs/rep\n", name.c_str(), r.p50_ms, r.p95_ms,
                r.items_per_second, r.allocations_per_rep);
    std::fflush(stdout);
    results_.push_back(r);
}

int Harness::finish() {
    if (options_.output_path.empty()) return 0;
    json::Value root = json::Value::object();
    root.set("schema", "victus.bench.v1");
    root.set("machine", machine_metadata());
    json::Value cfg = json::Value::object();
    cfg.set("warmup", options_.warmup);
    cfg.set("repetitions", options_.repetitions);
    root.set("config", cfg);
    json::Value cases = json::Value::array();
    for (const auto& r : results_) {
        json::Value c = json::Value::object();
        c.set("name", r.name);
        c.set("items_per_rep", r.items_per_rep);
        c.set("p50_ms", r.p50_ms);
        c.set("p95_ms", r.p95_ms);
        c.set("min_ms", r.min_ms);
        c.set("max_ms", r.max_ms);
        c.set("mean_ms", r.mean_ms);
        c.set("items_per_second", r.items_per_second);
        c.set("allocations_per_rep", r.allocations_per_rep);
        cases.push(c);
    }
    root.set("cases", cases);
    if (auto s = fs::write_text_atomic(options_.output_path, json::stringify(root)); !s.ok()) {
        std::fprintf(stderr, "failed to write results: %s\n", s.error().message.c_str());
        return 1;
    }
    std::printf("results written to %s\n", options_.output_path.c_str());
    return 0;
}

} // namespace victus::bench
