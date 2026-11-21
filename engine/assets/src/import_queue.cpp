// SPDX-License-Identifier: Apache-2.0
#include "victus/assets/import_queue.hpp"

#include "victus/assets/database.hpp"
#include "victus/core/log.hpp"

#include <algorithm>
#include <utility>

namespace victus::assets {

const char* import_state_name(ImportState s) noexcept {
    switch (s) {
    case ImportState::Queued: return "queued";
    case ImportState::Running: return "importing";
    case ImportState::Imported: return "imported";
    case ImportState::UpToDate: return "up to date";
    case ImportState::Failed: return "failed";
    case ImportState::Cancelled: return "cancelled";
    }
    return "";
}

Result<std::unique_ptr<ImportQueue>> ImportQueue::open(const fs::Path& project_root, std::uint32_t workers) {
    auto db = AssetDatabase::open(project_root);
    if (!db) return std::move(db).error();
    if ((*db)->packaged()) return make_error(ErrorCode::InvalidArgument, "an exported game has nothing to import");
    std::unique_ptr<ImportQueue> q(new ImportQueue(std::move(*db)));
    const std::uint32_t n = std::clamp(workers, 1u, 16u);
    q->workers_.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) q->workers_.emplace_back([p = q.get()] { p->worker(); });
    return q;
}

ImportQueue::ImportQueue(std::unique_ptr<AssetDatabase> db) : db_(std::move(db)) {}

ImportQueue::~ImportQueue() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        pending_.clear();
    }
    wake_.notify_all();
    for (auto& t : workers_) t.join();
}

ImportItem* ImportQueue::find_locked(const Uuid& uuid) {
    const auto it = std::find_if(items_.begin(), items_.end(), [&](const ImportItem& i) { return i.uuid == uuid; });
    return it == items_.end() ? nullptr : &*it;
}

void ImportQueue::queue_locked(ImportItem& item) {
    item.state = ImportState::Queued;
    item.error.clear();
    item.messages.clear();
    pending_.push_back(item.uuid);
}

std::size_t ImportQueue::enqueue(const std::vector<ImportRequest>& requests) {
    std::size_t added = 0;
    {
        std::lock_guard lock(mutex_);
        needs_refresh_ = true;
        for (const auto& r : requests) {
            if (r.uuid.is_nil()) continue;
            ImportItem* item = find_locked(r.uuid);
            if (!item) {
                items_.push_back({});
                item = &items_.back();
                item->uuid = r.uuid;
            } else if (!import_finished(item->state)) {
                continue;
            }
            if (!r.path.empty()) item->path = r.path;
            queue_locked(*item);
            ++added;
        }
    }
    if (added) wake_.notify_all();
    return added;
}

std::size_t ImportQueue::enqueue_all() {
    std::vector<ImportRequest> requests;
    {
        std::unique_lock db_lock(db_mutex_);
        const auto report = db_->refresh();
        for (const auto& e : report.errors) VICTUS_LOG_WARN("assets", "import queue: {}", e);
        for (const auto& r : db_->records())
            if (is_importable(r)) requests.push_back({r.uuid, r.path});
    }
    std::sort(requests.begin(), requests.end(), [](const ImportRequest& a, const ImportRequest& b) { return a.path < b.path; });
    return enqueue(requests);
}

std::size_t ImportQueue::cancel_all() {
    std::size_t n = 0;
    {
        std::lock_guard lock(mutex_);
        for (const Uuid& id : pending_)
            if (ImportItem* item = find_locked(id); item && item->state == ImportState::Queued) {
                item->state = ImportState::Cancelled;
                finished_.push_back(*item);
                ++n;
            }
        pending_.clear();
    }
    idle_.notify_all();
    return n;
}

bool ImportQueue::cancel(const Uuid& uuid) {
    {
        std::lock_guard lock(mutex_);
        ImportItem* item = find_locked(uuid);
        if (!item || item->state != ImportState::Queued) return false;
        item->state = ImportState::Cancelled;
        finished_.push_back(*item);
        pending_.erase(std::remove(pending_.begin(), pending_.end(), uuid), pending_.end());
    }
    idle_.notify_all();
    return true;
}

std::size_t ImportQueue::retry_failed() {
    std::size_t n = 0;
    {
        std::lock_guard lock(mutex_);
        needs_refresh_ = true;
        for (ImportItem& item : items_)
            if (item.state == ImportState::Failed || item.state == ImportState::Cancelled) {
                queue_locked(item);
                ++n;
            }
    }
    if (n) wake_.notify_all();
    return n;
}

bool ImportQueue::retry(const Uuid& uuid) {
    {
        std::lock_guard lock(mutex_);
        ImportItem* item = find_locked(uuid);
        if (!item || (item->state != ImportState::Failed && item->state != ImportState::Cancelled)) return false;
        needs_refresh_ = true;
        queue_locked(*item);
    }
    wake_.notify_all();
    return true;
}

void ImportQueue::clear_finished() {
    std::lock_guard lock(mutex_);
    std::erase_if(items_, [](const ImportItem& i) { return import_finished(i.state); });
}

void ImportQueue::set_paused(bool paused) {
    {
        std::lock_guard lock(mutex_);
        paused_ = paused;
    }
    wake_.notify_all();
}

bool ImportQueue::paused() const {
    std::lock_guard lock(mutex_);
    return paused_;
}

std::vector<ImportItem> ImportQueue::items() const {
    std::lock_guard lock(mutex_);
    return items_;
}

ImportProgress ImportQueue::progress() const {
    std::lock_guard lock(mutex_);
    ImportProgress p;
    p.total = items_.size();
    for (const ImportItem& i : items_) switch (i.state) {
        case ImportState::Queued: ++p.queued; break;
        case ImportState::Running:
            ++p.running;
            p.current.push_back(i.path);
            break;
        case ImportState::Imported: ++p.imported; break;
        case ImportState::UpToDate: ++p.up_to_date; break;
        case ImportState::Failed: ++p.failed; break;
        case ImportState::Cancelled: ++p.cancelled; break;
        }
    return p;
}

std::vector<ImportItem> ImportQueue::take_finished() {
    std::lock_guard lock(mutex_);
    return std::exchange(finished_, {});
}

bool ImportQueue::wait_idle(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return idle_.wait_for(lock, timeout, [&] { return (pending_.empty() || paused_) && running_ == 0; }) && pending_.empty();
}

void ImportQueue::refresh_if_needed() {
    {
        std::lock_guard lock(mutex_);
        if (!needs_refresh_) return;
        needs_refresh_ = false;
    }
    std::unique_lock db_lock(db_mutex_);
    const auto report = db_->refresh();
    for (const auto& e : report.errors) VICTUS_LOG_WARN("assets", "import queue: {}", e);
}

void ImportQueue::worker() {
    for (;;) {
        Uuid id;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stop_ || (!paused_ && !pending_.empty()); });
            if (stop_) return;
            id = pending_.front();
            pending_.pop_front();
            ImportItem* item = find_locked(id);
            if (!item || item->state != ImportState::Queued) continue;
            item->state = ImportState::Running;
            ++item->attempts;
            ++running_;
        }
        // Workers wait here while another one re-reads the project (exclusive lock).
        refresh_if_needed();
        const auto start = std::chrono::steady_clock::now();
        std::vector<std::string> messages;
        Result<bool> result = false;
        std::string path;
        {
            std::shared_lock db_lock(db_mutex_);
            if (const AssetRecord* rec = db_->find(id)) path = rec->path;
            result = db_->import(id, &messages);
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        {
            std::lock_guard lock(mutex_);
            --running_;
            if (ImportItem* item = find_locked(id)) {
                if (item->path.empty()) item->path = path.empty() ? id.to_string() : path;
                item->seconds = seconds;
                item->messages = std::move(messages);
                if (!result) {
                    item->state = ImportState::Failed;
                    item->error = result.error().code == ErrorCode::NotFound && path.empty()
                                      ? "the asset is no longer in the project"
                                      : result.error().message;
                } else {
                    item->state = *result ? ImportState::Imported : ImportState::UpToDate;
                }
                finished_.push_back(*item);
            }
        }
        idle_.notify_all();
    }
}

} // namespace victus::assets
