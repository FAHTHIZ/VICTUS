// SPDX-License-Identifier: Apache-2.0
#include "victus/core/profiler.hpp"

#include "victus/core/fs.hpp"
#include "victus/core/json.hpp"

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>
#include <numeric>

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

} // namespace victus::profiler
