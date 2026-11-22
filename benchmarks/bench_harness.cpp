// SPDX-License-Identifier: Apache-2.0
#include "bench\_harness.hpp"

#include "victus/core/build\_info.hpp"
#include "victus/core/cli.hpp"
#include "victus/core/fs.hpp"
#include "victus/core/memory.hpp"
#include "victus/core/profiler.hpp"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <thread>

namespace victus::bench {

Options parse\_options(int argc, char\*\* argv) {
    cli::Parser p(argv\[0\], "VICTUS benchmark runner");
    p.option("warmup", "warm-up repetitions", "3")
        .option("reps", "measured repetitions", "15")
        .option("json", "write machine-readable results to this path")
        .option("filter", "only run cases containing this text");
    if (auto s = p.parse(argc, argv); !s.ok()) {
        std::fprintf(stderr, "%s\\n%s", s.error().message.c\_str(), p.help().c\_str());
        std::exit(2);
    }
    const auto count = \[&amp;p\](const char\* key, int minimum) {
        const auto text = p.get(key);
        int value = 0;
        const auto\* end = text.data() + text.size();
        const auto parsed = std::from\_chars(text.data(), end, value);
        if (parsed.ec != std::errc{} || parsed.ptr != end || value &lt; minimum) {
            std::fprintf(stderr, "--%s requires a whole number of at least %d\\n", key, minimum);
            std::exit(2);
        }
        return value;
    };
    Options o;
    o.warmup = count("warmup", 0);
    o.repetitions = count("reps", 1);
    o.output\_path = p.get("json");
    o.filter = p.get("filter");
    return o;
}

json::Value machine\_metadata() {
    json::Value m = json::Value::object();
    m.set("victus\_version", build::version());
    m.set("git\_revision", build::git\_revision());
    m.set("platform", build::platform());
    m.set("architecture", build::architecture());
    m.set("compiler", build::compiler());
    m.set("build\_type", build::build\_type());
    m.set("hardware\_threads", static\_cast<std::uint32\_t>(std::thread::hardware\_concurrency()));
#if defined(**linux**)
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.rfind("model name", 0) == 0) {
            m.set("cpu", line.substr(line.find(':') + 2));
            break;
        }
    }
#endif
    m.set("allocation\_tracking", memory::allocation\_tracking\_enabled());
    m.set("timestamp\_unix", static\_cast<std::int64\_t>(std::time(nullptr)));
    return m;
}

void Harness::run(const std::string&amp; name, std::uint64\_t items, const std::function<void()>&amp; fn) {
    if (!options\_.filter.empty() &amp;&amp; name.find(options\_.filter) == std::string::npos) return;
    for (int i = 0; i &lt; options\_.warmup; ++i) fn();
    profiler::SampleWindow window(static\_cast<std::size\_t>(options\_.repetitions));
    std::uint64\_t allocs = 0;
    for (int i = 0; i &lt; options\_.repetitions;++i) {
        const auto a0 = memory::allocation\_stats().allocations;
        const auto t0 = std::chrono::steady\_clock::now();
        fn();
        const auto t1 = std::chrono::steady\_clock::now();
        allocs += memory::allocation\_stats().allocations - a0;
        window.add(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    CaseResult r;
    r.name = name;
    r.items\_per\_rep = items;
    r.p50\_ms = window.percentile(50);
    r.p95\_ms = window.percentile(95);
    r.min\_ms = window.min();
    r.max\_ms = window.max();
    r.mean\_ms = window.mean();
    r.items\_per\_second = r.p50\_ms &gt; 0 ? static\_cast<double>(items) / (r.p50\_ms / 1000.0) : 0;
    r.allocations\_per\_rep = static\_cast<double>(allocs) / options\_.repetitions;
    std::printf("%-44s p50 %9.3f ms  p95 %9.3f ms  %12.3e items/s  %8.1f allocs/rep\\n", name.c\_str(), r.p50\_ms, r.p95\_ms,
                r.items\_per\_second, r.allocations\_per\_rep);
    std::fflush(stdout);
    results\_.push\_back(r);
}

int Harness::finish() {
    if (results\_.empty()) {
        std::fprintf(stderr, "no benchmark cases ran (filter: '%s')\\n", options\_.filter.c\_str());
        return 2;
    }
    if (options\_.output\_path.empty()) return 0;
    json::Value root = json::Value::object();
    root.set("schema", "victus.bench.v1");
    root.set("machine", machine\_metadata());
    json::Value cfg = json::Value::object();
    cfg.set("warmup", options\_.warmup);
    cfg.set("repetitions", options\_.repetitions);
    root.set("config", cfg);
    json::Value cases = json::Value::array();
    for (const auto&amp; r : results\_) {
        json::Value c = json::Value::object();
        c.set("name", r.name);
        c.set("items\_per\_rep", r.items\_per\_rep);
        c.set("p50\_ms", r.p50\_ms);
        c.set("p95\_ms", r.p95\_ms);
        c.set("min\_ms", r.min\_ms);
        c.set("max\_ms", r.max\_ms);
        c.set("mean\_ms", r.mean\_ms);
        c.set("items\_per\_second", r.items\_per\_second);
        c.set("allocations\_per\_rep", r.allocations\_per\_rep);
        cases.push(c);
    }
    root.set("cases", cases);
    if (auto s = fs::write\_text\_atomic(options\_.output\_path, json::stringify(root)); !s.ok()) {
        std::fprintf(stderr, "failed to write results: %s\\n", s.error().message.c\_str());
        return 1;
    }
    std::printf("results written to %s\\n", options\_.output\_path.c\_str());
    return 0