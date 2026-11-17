// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "victus/core/result.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <utility>
#include <string>
#include <string_view>
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
// Takes the zones recorded so far without ending the capture (continuous profiling).
[[nodiscard]] std::vector<Zone> drain();
// A stable `const char*` for a runtime zone name (system names, render passes). Interned names
// live for the rest of the process; use it for a bounded set of names only.
[[nodiscard]] const char* intern(std::string_view name);

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

// Per-scope statistics over the frame window: time is the scope's total per frame (all calls,
// all threads; nested zones of the same name are counted once).
struct ScopeStats {
    std::string name;
    double mean_ms{0}, p95_ms{0}, max_ms{0}, last_ms{0};
    double calls_per_frame{0};
};

struct FrameReport {
    std::size_t frames{0}; // frames in the window
    double mean_ms{0}, p50_ms{0}, p95_ms{0}, p99_ms{0}, max_ms{0}, last_ms{0};
    std::vector<ScopeStats> scopes; // sorted by mean time, largest first
    // Extra counters the host attaches (memory, GPU resources, draw calls), written as-is.
    std::vector<std::pair<std::string, double>> counters;
};

// Continuous frame profiler (ADR-0061): keeps a capture running, drains it once per frame and
// aggregates frame times and per-scope totals over a rolling window. It can also keep every zone
// of the next N frames for a Chrome/Perfetto trace. One instance per process at a time.
class FrameProfiler {
public:
    explicit FrameProfiler(std::size_t window_frames = 300);
    ~FrameProfiler();
    FrameProfiler(const FrameProfiler&) = delete;
    FrameProfiler& operator=(const FrameProfiler&) = delete;

    // Marks a frame boundary: frame time is measured between consecutive calls, and the zones
    // finished since the previous call are attributed to the frame that just ended.
    void frame();
    void set_paused(bool paused) noexcept { paused_ = paused; }
    [[nodiscard]] bool paused() const noexcept { return paused_; }
    void reset();

    [[nodiscard]] FrameReport report() const;
    [[nodiscard]] const SampleWindow& frame_times() const noexcept { return frame_ms_; }
    [[nodiscard]] std::size_t frames_profiled() const noexcept { return frames_; }

    // Keeps all zones of the next `frames` frames; recording() is true until they are collected.
    void record(std::size_t frames);
    // Ends a recording early; take_recording() then returns what was kept.
    void stop_recording() noexcept {
        if (record_left_ > 0) {
            record_left_ = 0;
            record_done_ = true;
        }
    }
    [[nodiscard]] bool recording() const noexcept { return record_left_ > 0; }
    // The finished recording (empty while still recording); clears it.
    [[nodiscard]] std::vector<Zone> take_recording();

private:
    struct Scope {
        SampleWindow ms;
        SampleWindow calls;
    };
    std::size_t window_;
    SampleWindow frame_ms_;
    std::map<std::string, Scope, std::less<>> scopes_;
    std::uint64_t last_ns_{0};
    std::size_t frames_{0};
    bool paused_{false};
    std::size_t record_left_{0};
    std::vector<Zone> recorded_;
    bool record_done_{false};
};

// JSON summary of a report (CLI `--profile` output and editor export).
[[nodiscard]] std::string report_json(const FrameReport& report);

} // namespace victus::profiler

#define VICTUS_PROFILE_CONCAT2(a, b) a##b
#define VICTUS_PROFILE_CONCAT(a, b) VICTUS_PROFILE_CONCAT2(a, b)
#define VICTUS_PROFILE_SCOPE(name) ::victus::profiler::ScopedZone VICTUS_PROFILE_CONCAT(victus_zone_, __LINE__)(name)
