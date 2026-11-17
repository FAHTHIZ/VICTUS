// SPDX-License-Identifier: Apache-2.0
#include "victus/core/profiler.hpp"

#include "victus/core/fs.hpp"
#include "victus/core/json.hpp"

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>
#include <numeric>
#include <unordered_set>

namespace victus::profiler {

namespace {
struct ThreadBuffer {
    std::mutex mutex;
    std::vector<Zone> zones;
    std::uint32_t id{0};
    std::string name;
    std::uint16_t depth{0};
    std::uint64_t generation{0};
};

std::atomic<bool> g_capturing{false};
std::atomic<std::uint64_t> g_generation{0};
std::atomic<std::uint32_t> g_next_thread{1};
std::mutex g_registry_mutex;
std::vector<std::shared_ptr<ThreadBuffer>>& registry() {
    static std::vector<std::shared_ptr<ThreadBuffer>> r;
    return r;
}
std::vector<std::pair<std::uint32_t, std::string>> g_thread_names;

ThreadBuffer& local_buffer() {
    thread_local std::shared_ptr<ThreadBuffer> buffer = [] {
        auto b = std::make_shared<ThreadBuffer>();
        b->id = g_next_thread.fetch_add(1);
        std::lock_guard lock(g_registry_mutex);
        registry().push_back(b);
        return b;
    }();
    return *buffer;
}
constexpr std::size_t kMaxZonesPerThread = 1u << 20;
} // namespace

void begin_capture() {
    std::lock_guard lock(g_registry_mutex);
    for (auto& b : registry()) {
        std::lock_guard bl(b->mutex);
        b->zones.clear();
    }
    g_generation.fetch_add(1);
    g_capturing.store(true, std::memory_order_release);
}

std::vector<Zone> end_capture() {
    g_capturing.store(false, std::memory_order_release);
    std::vector<Zone> all;
    std::lock_guard lock(g_registry_mutex);
    for (auto& b : registry()) {
        std::lock_guard bl(b->mutex);
        all.insert(all.end(), b->zones.begin(), b->zones.end());
        b->zones.clear();
    }
    // Coarse clocks (100 ns on Windows) give nested zones equal begin times: parents first.
    std::stable_sort(all.begin(), all.end(), [](const Zone& a, const Zone& b) {
        if (a.begin_ns != b.begin_ns) return a.begin_ns < b.begin_ns;
        if (a.thread != b.thread) return a.thread < b.thread;
        return a.depth < b.depth;
    });
    return all;
}

std::vector<Zone> drain() {
    std::vector<Zone> all;
    std::lock_guard lock(g_registry_mutex);
    for (auto& b : registry()) {
        std::lock_guard bl(b->mutex);
        all.insert(all.end(), b->zones.begin(), b->zones.end());
        b->zones.clear();
    }
    std::stable_sort(all.begin(), all.end(), [](const Zone& a, const Zone& b) {
        if (a.begin_ns != b.begin_ns) return a.begin_ns < b.begin_ns;
        if (a.thread != b.thread) return a.thread < b.thread;
        return a.depth < b.depth;
    });
    return all;
}

const char* intern(std::string_view name) {
    static std::mutex m;
    static std::unordered_set<std::string> names; // node-based: element addresses are stable
    std::lock_guard lock(m);
    return names.emplace(name).first->c_str();
}

bool capturing() noexcept { return g_capturing.load(std::memory_order_relaxed); }

void set_thread_name(const char* name) {
    ThreadBuffer& b = local_buffer();
    b.name = name;
    std::lock_guard lock(g_registry_mutex);
    g_thread_names.emplace_back(b.id, name);
}

ScopedZone::ScopedZone(const char* name) noexcept : name_(name), begin_(0), active_(capturing()) {
    if (active_) {
        ++local_buffer().depth;
        begin_ = now_ns();
    }
}

ScopedZone::~ScopedZone() {
    if (!active_) return;
    const std::uint64_t end = now_ns();
    ThreadBuffer& b = local_buffer();
    --b.depth;
    if (!capturing()) return;
    std::lock_guard lock(b.mutex);
    if (b.zones.size() < kMaxZonesPerThread) b.zones.push_back({name_, begin_, end, b.id, b.depth});
}

Status write_chrome_trace(const std::filesystem::path& path, const std::vector<Zone>& zones) {
    json::Value events = json::Value::array();
    const std::uint64_t origin = zones.empty() ? 0 : zones.front().begin_ns;
    {
        std::lock_guard lock(g_registry_mutex);
        for (const auto& [id, name] : g_thread_names) {
            json::Value meta = json::Value::object();
            meta.set("name", "thread_name");
            meta.set("ph", "M");
            meta.set("pid", 1);
            meta.set("tid", id);
            json::Value args = json::Value::object();
            args.set("name", name);
            meta.set("args", std::move(args));
            events.push(std::move(meta));
        }
    }
    for (const Zone& z : zones) {
        json::Value e = json::Value::object();
        e.set("name", z.name);
        e.set("ph", "X");
        e.set("pid", 1);
        e.set("tid", z.thread);
        e.set("ts", static_cast<double>(z.begin_ns - origin) / 1000.0);
        e.set("dur", static_cast<double>(z.end_ns - z.begin_ns) / 1000.0);
        events.push(std::move(e));
    }
    json::Value root = json::Value::object();
    root.set("traceEvents", std::move(events));
    root.set("displayTimeUnit", "ms");
    return fs::write_text_atomic(path, json::stringify(root, false));
}

void SampleWindow::add(double value) {
    last_ = value;
    if (capacity_ == 0) return;
    if (samples_.size() < capacity_) {
        samples_.push_back(value);
    } else {
        samples_[head_] = value;
        head_ = (head_ + 1) % capacity_;
    }
}

double SampleWindow::percentile(double p) const {
    if (samples_.empty()) return 0.0;
    std::vector<double> sorted = samples_;
    std::sort(sorted.begin(), sorted.end());
    p = std::clamp(p, 0.0, 100.0);
    // Nearest-rank with linear interpolation between neighbours.
    const double rank = p / 100.0 * static_cast<double>(sorted.size() - 1);
    const auto lo = static_cast<std::size_t>(rank);
    const std::size_t hi = std::min(lo + 1, sorted.size() - 1);
    const double t = rank - static_cast<double>(lo);
    return sorted[lo] + (sorted[hi] - sorted[lo]) * t;
}

double SampleWindow::mean() const {
    if (samples_.empty()) return 0.0;
    return std::accumulate(samples_.begin(), samples_.end(), 0.0) / static_cast<double>(samples_.size());
}
double SampleWindow::min() const { return samples_.empty() ? 0.0 : *std::min_element(samples_.begin(), samples_.end()); }
double SampleWindow::max() const { return samples_.empty() ? 0.0 : *std::max_element(samples_.begin(), samples_.end()); }

std::vector<double> SampleWindow::ordered() const {
    std::vector<double> out;
    out.reserve(samples_.size());
    if (samples_.size() < capacity_) return samples_;
    for (std::size_t i = 0; i < samples_.size(); ++i) out.push_back(samples_[(head_ + i) % samples_.size()]);
    return out;
}

// ---- FrameProfiler --------------------------------------------------------------------------------

FrameProfiler::FrameProfiler(std::size_t window_frames) : window_(std::max<std::size_t>(window_frames, 1)), frame_ms_(window_) {
    if (!capturing()) begin_capture();
}

FrameProfiler::~FrameProfiler() {
    if (capturing()) (void)end_capture();
}

void FrameProfiler::reset() {
    frame_ms_.clear();
    scopes_.clear();
    frames_ = 0;
    last_ns_ = 0;
}

void FrameProfiler::frame() {
    if (!capturing()) begin_capture(); // someone ended the capture (e.g. a one-off trace)
    const std::uint64_t now = now_ns();
    std::vector<Zone> zones = drain();
    const bool have_frame = last_ns_ != 0;
    const double frame_ms = have_frame ? static_cast<double>(now - last_ns_) / 1e6 : 0.0;
    last_ns_ = now;
    if (record_left_ > 0) {
        recorded_.insert(recorded_.end(), zones.begin(), zones.end());
        if (--record_left_ == 0) record_done_ = true;
    }
    if (paused_ || !have_frame) return;
    frame_ms_.add(frame_ms);
    ++frames_;
    // Per name: total time with nested same-name zones counted once (per thread, outermost).
    struct Acc {
        double ms{0};
        double calls{0};
    };
    std::stable_sort(zones.begin(), zones.end(), [](const Zone& x, const Zone& y) {
        return x.thread != y.thread ? x.thread < y.thread : x.begin_ns < y.begin_ns;
    });
    std::map<std::string_view, Acc> acc;
    std::map<std::pair<std::uint32_t, std::string_view>, std::uint64_t> open_until; // thread+name -> end of outermost
    for (const Zone& z : zones) {
        const std::string_view name(z.name ? z.name : "?");
        Acc& a = acc[name];
        a.calls += 1;
        auto& until = open_until[{z.thread, name}];
        if (z.begin_ns >= until) {
            a.ms += static_cast<double>(z.end_ns - z.begin_ns) / 1e6;
            until = z.end_ns;
        }
    }
    for (const auto& [name, a] : acc) {
        auto it = scopes_.find(name);
        if (it != scopes_.end()) continue;
        // A scope first seen now gets zero samples for the earlier frames of the window, so every
        // scope is averaged over the same frames.
        Scope sc{SampleWindow(window_), SampleWindow(window_)};
        for (std::size_t i = 1; i < frame_ms_.count(); ++i) {
            sc.ms.add(0.0);
            sc.calls.add(0.0);
        }
        scopes_.emplace(std::string(name), std::move(sc));
    }
    for (auto& [name, sc] : scopes_) {
        const auto it = acc.find(name);
        sc.ms.add(it == acc.end() ? 0.0 : it->second.ms);
        sc.calls.add(it == acc.end() ? 0.0 : it->second.calls);
    }
}

FrameReport FrameProfiler::report() const {
    FrameReport r;
    r.frames = frame_ms_.count();
    r.mean_ms = frame_ms_.mean();
    r.p50_ms = frame_ms_.percentile(50);
    r.p95_ms = frame_ms_.percentile(95);
    r.p99_ms = frame_ms_.percentile(99);
    r.max_ms = frame_ms_.max();
    r.last_ms = frame_ms_.last();
    for (const auto& [name, sc] : scopes_)
        r.scopes.push_back({name, sc.ms.mean(), sc.ms.percentile(95), sc.ms.max(), sc.ms.last(), sc.calls.mean()});
    std::stable_sort(r.scopes.begin(), r.scopes.end(), [](const ScopeStats& a, const ScopeStats& b) { return a.mean_ms > b.mean_ms; });
    return r;
}

void FrameProfiler::record(std::size_t frames) {
    recorded_.clear();
    record_done_ = false;
    record_left_ = frames;
}

std::vector<Zone> FrameProfiler::take_recording() {
    if (!record_done_) return {};
    record_done_ = false;
    return std::move(recorded_);
}

std::string report_json(const FrameReport& report) {
    json::Value root = json::Value::object();
    root.set("frames", static_cast<double>(report.frames));
    json::Value frame = json::Value::object();
    frame.set("mean_ms", report.mean_ms);
    frame.set("p50_ms", report.p50_ms);
    frame.set("p95_ms", report.p95_ms);
    frame.set("p99_ms", report.p99_ms);
    frame.set("max_ms", report.max_ms);
    root.set("frame", std::move(frame));
    json::Value scopes = json::Value::array();
    for (const auto& s : report.scopes) {
        json::Value v = json::Value::object();
        v.set("name", s.name);
        v.set("mean_ms", s.mean_ms);
        v.set("p95_ms", s.p95_ms);
        v.set("max_ms", s.max_ms);
        v.set("calls_per_frame", s.calls_per_frame);
        scopes.push(std::move(v));
    }
    root.set("scopes", std::move(scopes));
    if (!report.counters.empty()) {
        json::Value counters = json::Value::object();
        for (const auto& [name, value] : report.counters) counters.set(name, value);
        root.set("counters", std::move(counters));
    }
    return json::stringify(root, true) + "\n";
}

} // namespace victus::profiler
