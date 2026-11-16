// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "victus/core/result.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace victus::profiler {

[[nodiscard]] inline std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Zone {
    const char* name;      // must have static storage duration (string literal)
    std::uint64_t begin_ns;
    std::uint64_t end_ns;
    std::uint32_t thread;
    std::uint16_t depth;
};

// Global capture control. Zones are recorded into thread-local buffers only while a
// capture is active, so instrumentation costs one relaxed atomic load otherwise.
void begin_capture();
[[nodiscard]] std::vector<Zone> end_capture();
[[nodiscard]] bool capturing() noexcept;
void set_thread_name(const char* name);

// Chrome/Perfetto trace-event JSON (open in https://ui.perfetto.dev or chrome://tracing).
[[nodiscard]] Status write_chrome_trace(const std::filesystem::path& path, const std::vector<Zone>& zones);

class ScopedZone {
public:
    explicit ScopedZone(const char* name) noexcept;
    ~ScopedZone();
    ScopedZone(const ScopedZone&) = delete;
    ScopedZone& operator=(const ScopedZone&) = delete;

private:
    const char* name_;
    std::uint64_t begin_;
    bool active_;
};

// Rolling sample window with percentile queries for frame/tick timings.
class SampleWindow {
public:
    explicit SampleWindow(std::size_t capacity = 600) : capacity_(capacity) { samples_.reserve(capacity); }
    void add(double value);
    [[nodiscard]] std::size_t count() const noexcept { return samples_.size(); }
    [[nodiscard]] double percentile(double p) const; // p in [0,100]
    [[nodiscard]] double mean() const;
    [[nodiscard]] double min() const;
    [[nodiscard]] double max() const;
    [[nodiscard]] double last() const noexcept { return last_; }
    [[nodiscard]] std::vector<double> ordered() const; // oldest → newest
    void clear() noexcept {
        samples_.clear();
        head_ = 0;
    }

private:
    std::vector<double> samples_;
    std::size_t capacity_;
    std::size_t head_{0};
    double last_{0.0};
};

} // namespace victus::profiler

#define VICTUS_PROFILE_CONCAT2(a, b) a##b
#define VICTUS_PROFILE_CONCAT(a, b) VICTUS_PROFILE_CONCAT2(a, b)
#define VICTUS_PROFILE_SCOPE(name) ::victus::profiler::ScopedZone VICTUS_PROFILE_CONCAT(victus_zone_, __LINE__)(name)
