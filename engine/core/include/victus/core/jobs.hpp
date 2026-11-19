// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace victus::jobs {

enum class Priority : std::uint8_t {
    High,      // frame-critical work (simulation, render extraction)
    Normal,
    Background // streaming, asset import, cooking; never starves High work
};

// Completion counter. A job group is done when its counter reaches zero.
class Counter {
public:
    [[nodiscard]] bool done() const noexcept { return value_.load(std::memory_order_acquire) == 0; }
    [[nodiscard]] std::int64_t pending() const noexcept { return value_.load(std::memory_order_acquire); }

private:
    friend class JobSystem;
    std::atomic<std::int64_t> value_{0};
};

using JobFn = std::function<void()>;

struct JobSystemStats {
    std::uint64_t executed{0};
    std::uint64_t stolen{0};
    std::uint32_t workers{0};
};

// Work-stealing thread pool. Each worker owns a deque (LIFO for locality); idle workers
// steal FIFO from others. Threads waiting on a Counter help execute jobs instead of
// blocking, so nested parallelism cannot deadlock the pool.
class JobSystem {
public:
    // worker_count == 0 selects hardware_concurrency()-1 (minimum 1).
    explicit JobSystem(std::uint32_t worker_count = 0);
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    void submit(JobFn fn, Counter* counter = nullptr, Priority priority = Priority::Normal);
    // Blocks until counter reaches zero, executing other jobs meanwhile (on threads outside the
    // pool, only High/Normal jobs: Background work is left to the workers).
    void wait(Counter& counter);
    // Runs body(i) for i in [begin, end) split into chunks of at least `grain` items.
    void parallel_for(std::size_t begin, std::size_t end, std::size_t grain,
                      const std::function<void(std::size_t, std::size_t)>& body);

    [[nodiscard]] std::uint32_t worker_count() const noexcept { return static_cast<std::uint32_t>(workers_.size()); }
    [[nodiscard]] JobSystemStats stats() const noexcept;
    // Index of the current thread inside this system: 0 = external thread, 1..N = workers.
    [[nodiscard]] std::uint32_t current_thread_index() const noexcept;

private:
    struct Job {
        JobFn fn;
        Counter* counter;
    };
    struct Queue {
        std::mutex mutex;
        std::deque<Job> jobs;
    };
    bool try_run_one(std::uint32_t self, bool allow_background = true);
    bool pop_local(std::uint32_t self, Job& out);
    bool steal(std::uint32_t self, Job& out);
    void execute(Job& job);
    void worker_main(std::uint32_t index);

    std::vector<std::thread> workers_;
    std::vector<std::unique_ptr<Queue>> local_; // index 0 = external injection queue
    Queue high_, background_;
    std::mutex sleep_mutex_;
    std::condition_variable sleep_cv_;
    std::atomic<std::int64_t> queued_{0};
    std::atomic<bool> running_{true};
    std::atomic<std::uint64_t> executed_{0};
    std::atomic<std::uint64_t> stolen_{0};
};

// Static dependency graph of named tasks, executed on a JobSystem. Used by the ECS
// scheduler and frame pipeline. Tasks run once all predecessors complete.
class TaskGraph {
public:
    using TaskId = std::uint32_t;
    TaskId add(std::string name, JobFn fn, Priority priority = Priority::High);
    // `before` must complete before `after` starts.
    void precede(TaskId before, TaskId after);
    // Validates (cycle detection) and executes the whole graph, blocking until done.
    [[nodiscard]] bool run(JobSystem& jobs);
    [[nodiscard]] std::size_t size() const noexcept { return tasks_.size(); }
    [[nodiscard]] const std::string& name(TaskId id) const { return tasks_[id].name; }
    [[nodiscard]] bool has_cycle() const;

private:
    struct Task {
        std::string name;
        JobFn fn;
        Priority priority;
        std::vector<TaskId> successors;
        std::uint32_t predecessor_count{0};
    };
    std::vector<Task> tasks_;
};

} // namespace victus::jobs
