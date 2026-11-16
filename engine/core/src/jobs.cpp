// SPDX-License-Identifier: Apache-2.0
#include "victus/core/jobs.hpp"

#include <algorithm>

namespace victus::jobs {

namespace {
thread_local const JobSystem* t_owner = nullptr;
thread_local std::uint32_t t_index = 0;
} // namespace

JobSystem::JobSystem(std::uint32_t worker_count) {
    if (worker_count == 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        worker_count = hw > 1 ? hw - 1 : 1;
    }
    local_.reserve(worker_count + 1);
    for (std::uint32_t i = 0; i <= worker_count; ++i) local_.push_back(std::make_unique<Queue>());
    workers_.reserve(worker_count);
    for (std::uint32_t i = 1; i <= worker_count; ++i) workers_.emplace_back([this, i] { worker_main(i); });
}

JobSystem::~JobSystem() {
    {
        std::lock_guard lock(sleep_mutex_);
        running_.store(false, std::memory_order_release);
    }
    sleep_cv_.notify_all();
    for (auto& t : workers_) t.join();
    // Drain anything submitted but never executed so counters are released.
    while (try_run_one(0)) {
    }
}

std::uint32_t JobSystem::current_thread_index() const noexcept { return t_owner == this ? t_index : 0; }

void JobSystem::submit(JobFn fn, Counter* counter, Priority priority) {
    if (counter) counter->value_.fetch_add(1, std::memory_order_acq_rel);
    Queue* q = nullptr;
    if (priority == Priority::High) q = &high_;
    else if (priority == Priority::Background) q = &background_;
    else q = local_[current_thread_index()].get();
    {
        std::lock_guard lock(q->mutex);
        q->jobs.push_back({std::move(fn), counter});
    }
    queued_.fetch_add(1, std::memory_order_release);
    {
        // Lock pairs with the sleep predicate check to avoid lost wake-ups.
        std::lock_guard lock(sleep_mutex_);
    }
    sleep_cv_.notify_one();
}

bool JobSystem::pop_local(std::uint32_t self, Job& out) {
    Queue& q = *local_[self];
    std::lock_guard lock(q.mutex);
    if (q.jobs.empty()) return false;
    out = std::move(q.jobs.back());
    q.jobs.pop_back();
    return true;
}

bool JobSystem::steal(std::uint32_t self, Job& out) {
    const std::size_t n = local_.size();
    for (std::size_t k = 1; k <= n; ++k) {
        const std::size_t victim = (self + k) % n;
        if (victim == self) continue;
        Queue& q = *local_[victim];
        std::unique_lock lock(q.mutex, std::try_to_lock);
        if (!lock.owns_lock() || q.jobs.empty()) continue;
        out = std::move(q.jobs.front());
        q.jobs.pop_front();
        stolen_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

bool JobSystem::try_run_one(std::uint32_t self) {
    Job job;
    auto take_from = [&](Queue& q) {
        std::lock_guard lock(q.mutex);
        if (q.jobs.empty()) return false;
        job = std::move(q.jobs.front());
        q.jobs.pop_front();
        return true;
    };
    if (take_from(high_) || pop_local(self, job) || steal(self, job) || take_from(background_)) {
        queued_.fetch_sub(1, std::memory_order_acq_rel);
        execute(job);
        return true;
    }
    return false;
}

void JobSystem::execute(Job& job) {
    job.fn();
    executed_.fetch_add(1, std::memory_order_relaxed);
    if (job.counter) job.counter->value_.fetch_sub(1, std::memory_order_acq_rel);
}

void JobSystem::worker_main(std::uint32_t index) {
    t_owner = this;
    t_index = index;
    while (running_.load(std::memory_order_acquire)) {
        if (try_run_one(index)) continue;
        std::unique_lock lock(sleep_mutex_);
        sleep_cv_.wait(lock, [&] {
            return !running_.load(std::memory_order_acquire) || queued_.load(std::memory_order_acquire) > 0;
        });
    }
}

void JobSystem::wait(Counter& counter) {
    const std::uint32_t self = current_thread_index();
    std::uint32_t idle_spins = 0;
    while (!counter.done()) {
        if (try_run_one(self)) {
            idle_spins = 0;
            continue;
        }
        // Remaining work is running on other threads; back off progressively.
        if (++idle_spins < 64) std::this_thread::yield();
        else std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

void JobSystem::parallel_for(std::size_t begin, std::size_t end, std::size_t grain,
                             const std::function<void(std::size_t, std::size_t)>& body) {
    if (end <= begin) return;
    grain = std::max<std::size_t>(grain, 1);
    const std::size_t count = end - begin;
    const std::size_t max_chunks = static_cast<std::size_t>(worker_count() + 1) * 4;
    const std::size_t chunk = std::max(grain, (count + max_chunks - 1) / max_chunks);
    if (count <= chunk) {
        body(begin, end);
        return;
    }
    Counter counter;
    for (std::size_t s = begin + chunk; s < end; s += chunk) {
        const std::size_t e = std::min(end, s + chunk);
        submit([&body, s, e] { body(s, e); }, &counter, Priority::High);
    }
    body(begin, std::min(end, begin + chunk));
    wait(counter);
}

JobSystemStats JobSystem::stats() const noexcept {
    return {executed_.load(std::memory_order_relaxed), stolen_.load(std::memory_order_relaxed), worker_count()};
}

TaskGraph::TaskId TaskGraph::add(std::string name, JobFn fn, Priority priority) {
    tasks_.push_back({std::move(name), std::move(fn), priority, {}, 0});
    return static_cast<TaskId>(tasks_.size() - 1);
}

void TaskGraph::precede(TaskId before, TaskId after) {
    tasks_[before].successors.push_back(after);
    ++tasks_[after].predecessor_count;
}

bool TaskGraph::has_cycle() const {
    std::vector<std::uint32_t> indeg(tasks_.size());
    for (std::size_t i = 0; i < tasks_.size(); ++i) indeg[i] = tasks_[i].predecessor_count;
    std::vector<TaskId> ready;
    for (TaskId i = 0; i < tasks_.size(); ++i)
        if (indeg[i] == 0) ready.push_back(i);
    std::size_t visited = 0;
    while (!ready.empty()) {
        const TaskId t = ready.back();
        ready.pop_back();
        ++visited;
        for (const TaskId s : tasks_[t].successors)
            if (--indeg[s] == 0) ready.push_back(s);
    }
    return visited != tasks_.size();
}

bool TaskGraph::run(JobSystem& jobs) {
    if (tasks_.empty()) return true;
    if (has_cycle()) return false;
    auto remaining = std::make_unique<std::atomic<std::uint32_t>[]>(tasks_.size());
    for (std::size_t i = 0; i < tasks_.size(); ++i) remaining[i].store(tasks_[i].predecessor_count);
    Counter counter;
    std::function<void(TaskId)> launch;
    launch = [&](TaskId id) {
        jobs.submit(
            [&, id] {
                tasks_[id].fn();
                for (const TaskId s : tasks_[id].successors)
                    if (remaining[s].fetch_sub(1, std::memory_order_acq_rel) == 1) launch(s);
            },
            &counter, tasks_[id].priority);
    };
    for (TaskId i = 0; i < tasks_.size(); ++i)
        if (tasks_[i].predecessor_count == 0) launch(i);
    jobs.wait(counter);
    return true;
}

} // namespace victus::jobs
