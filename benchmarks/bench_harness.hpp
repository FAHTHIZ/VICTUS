// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "victus/core/json.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace victus::bench {

struct Options {
    int warmup{3};
    int repetitions{15};
    std::string output_path; // JSON results when non-empty
    std::string filter;      // substring filter on case names
};

struct CaseResult {
    std::string name;
    std::uint64_t items_per_rep{0};
    double p50_ms{0}, p95_ms{0}, min_ms{0}, max_ms{0}, mean_ms{0};
    double items_per_second{0};
    double allocations_per_rep{0};
};

class Harness {
public:
    explicit Harness(Options options) : options_(std::move(options)) {}
    // `fn` runs one repetition and must process `items` work units.
    void run(const std::string& name, std::uint64_t items, const std::function<void()>& fn);
    // Prints a table and writes JSON (with machine/build metadata) if requested. Returns exit code.
    int finish();

private:
    Options options_;
    std::vector<CaseResult> results_;
};

Options parse_options(int argc, char** argv);

// Prevents the optimizer from discarding a computed value.
template <typename T>
inline void do_not_optimize(const T& value) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "r,m"(value) : "memory");
#else
    static volatile const void* sink;
    sink = &value;
#endif
}
json::Value machine_metadata();

} // namespace victus::bench
