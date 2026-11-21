// SPDX-License-Identifier: Apache-2.0
// Background asset import queue (ADR-0189). Imports run on worker threads with their own
// AssetDatabase on the same project, so the caller's database (and the UI using it) is never
// blocked or mutated concurrently; cooked files are written atomically and the caller's
// database sees them as up to date. Items report progress, timings, importer messages and
// errors per file; queued items can be cancelled, failed and cancelled ones retried.
//
// Cancellation is cooperative at item granularity: an import already running finishes (the
// importers are not interruptible), everything still queued is dropped.
#pragma once

#include "victus/core/fs.hpp"
#include "victus/core/result.hpp"
#include "victus/core/hash.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

namespace victus::assets {

class AssetDatabase;

enum class ImportState : std::uint8_t {
    Queued,
    Running,
    Imported,  // cooked anew (or linked from the shared cache)
    UpToDate,  // the cooked file already matched the source and settings
    Failed,    // `error` says why; the previous cooked file (if any) is kept
    Cancelled, // dropped from the queue before it ran
};
[[nodiscard]] const char* import_state_name(ImportState s) noexcept;
[[nodiscard]] constexpr bool import_finished(ImportState s) noexcept { return s != ImportState::Queued && s != ImportState::Running; }

struct ImportItem {
    Uuid uuid;
    std::string path; // project-relative asset path, for display
    ImportState state{ImportState::Queued};
    std::string error;                 // Failed: the importer's message
    std::vector<std::string> messages; // importer warnings and notes
    double seconds{0.0};               // wall time of the last attempt
    std::uint32_t attempts{0};
};

struct ImportProgress {
    std::size_t total{0};    // items currently listed (queued, running and finished)
    std::size_t queued{0};   // waiting
    std::size_t running{0};  // being imported now
    std::size_t imported{0}; // finished: imported
    std::size_t up_to_date{0};
    std::size_t failed{0};
    std::size_t cancelled{0};
    std::vector<std::string> current; // paths being imported now
    [[nodiscard]] bool busy() const noexcept { return queued + running > 0; }
    // Finished share of the work listed since the queue was last idle and cleared (0..1).
    [[nodiscard]] float fraction() const noexcept {
        return total ? static_cast<float>(total - queued - running) / static_cast<float>(total) : 1.0f;
    }
};

struct ImportRequest {
    Uuid uuid;
    std::string path; // optional; resolved from the queue's database when empty
};

class ImportQueue {
public:
    // Opens a private AssetDatabase on `project_root` and starts `workers` threads (at least 1).
    [[nodiscard]] static Result<std::unique_ptr<ImportQueue>> open(const fs::Path& project_root, std::uint32_t workers = 1);
    ~ImportQueue(); // drops queued items and joins the workers (a running import finishes first)
    ImportQueue(const ImportQueue&) = delete;
    ImportQueue& operator=(const ImportQueue&) = delete;

    // Adds assets to the queue; an asset already queued or running is not added twice, a finished
    // one is queued again. The queue re-reads the project's .meta files before its next import, so
    // assets and settings the caller just created or changed are seen. Returns how many were added.
    std::size_t enqueue(const std::vector<ImportRequest>& requests);
    // Every importable asset in the project (after re-reading it). Up-to-date ones finish quickly.
    std::size_t enqueue_all();
    // Queued items become Cancelled (running ones finish). Returns how many were cancelled.
    std::size_t cancel_all();
    bool cancel(const Uuid& uuid);
    // Failed and cancelled items are queued again. Returns how many.
    std::size_t retry_failed();
    bool retry(const Uuid& uuid);
    // Forgets finished items (the list and progress then cover only pending work).
    void clear_finished();
    // While paused, workers start no new imports.
    void set_paused(bool paused);
    [[nodiscard]] bool paused() const;

    [[nodiscard]] std::vector<ImportItem> items() const;
    [[nodiscard]] ImportProgress progress() const;
    // Items that finished since the previous call (in completion order), for callers that need to
    // react on their own thread: evict GPU copies, refresh thumbnails, report errors.
    [[nodiscard]] std::vector<ImportItem> take_finished();
    // Blocks until nothing is queued or running, or the timeout passes. True when idle.
    bool wait_idle(std::chrono::milliseconds timeout = std::chrono::minutes(10));

    [[nodiscard]] std::uint32_t worker_count() const noexcept { return static_cast<std::uint32_t>(workers_.size()); }

private:
    explicit ImportQueue(std::unique_ptr<AssetDatabase> db);
    void worker();
    void refresh_if_needed();
    ImportItem* find_locked(const Uuid& uuid);
    void queue_locked(ImportItem& item);

    std::unique_ptr<AssetDatabase> db_;
    // Imports share the database; re-reading the project's .meta files needs it alone.
    mutable std::shared_mutex db_mutex_;
    bool needs_refresh_{true}; // guarded by mutex_

    mutable std::mutex mutex_;
    std::condition_variable wake_;  // workers: work or stop
    std::condition_variable idle_;  // wait_idle
    std::vector<ImportItem> items_; // listed items, in the order first queued
    std::deque<Uuid> pending_;      // queued, in order
    std::vector<ImportItem> finished_; // since the last take_finished
    std::size_t running_{0};
    bool paused_{false};
    bool stop_{false};
    std::vector<std::thread> workers_;
};

} // namespace victus::assets
