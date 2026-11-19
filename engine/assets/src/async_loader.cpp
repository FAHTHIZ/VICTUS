// SPDX-License-Identifier: Apache-2.0
#include "victus/assets/async_loader.hpp"

#include "victus/core/profiler.hpp"

#include <algorithm>
#include <atomic>
#include <optional>

namespace victus::assets {

std::size_t upload_bytes(const CookedTexture& texture) noexcept {
    std::size_t bytes = 0;
    if (!texture.compressed.empty())
        for (const auto& level : texture.compressed.front().levels) bytes += level.size();
    else
        for (const auto& mip : texture.mips) bytes += mip.pixels.size();
    return bytes + texture.hdr_pixels.size() * sizeof(std::uint16_t);
}

std::size_t upload_bytes(const LoadPayload& payload) noexcept {
    if (const auto* t = std::get_if<CookedTexture>(&payload)) return upload_bytes(*t);
    return upload_bytes(std::get<CookedModel>(payload));
}

std::size_t upload_bytes(const CookedModel& m) noexcept {
    std::size_t bytes = 0;
    for (const auto& mesh : m.meshes) {
        const render::MeshData& d = mesh.data;
        bytes += d.vertices.size() * sizeof(render::Vertex3D) + d.indices.size() * sizeof(std::uint32_t) +
                 d.skin.size() * sizeof(render::SkinVertex);
        for (const auto& lod : d.lods) bytes += lod.indices.size() * sizeof(std::uint32_t);
    }
    for (const auto& t : m.textures) bytes += upload_bytes(t);
    return bytes;
}

struct AsyncLoader::Job {
    Uuid uuid;
    LoadType type{LoadType::Texture};
    std::atomic<bool> done{false};
    std::optional<Result<LoadPayload>> result; // written by the worker before `done`
};

AsyncLoader::AsyncLoader(jobs::JobSystem* jobs, LoadFetch fetch, LoaderSettings settings)
    : jobs_(jobs), fetch_(std::move(fetch)), settings_(settings) {}

AsyncLoader::~AsyncLoader() { wait_idle(); }

void AsyncLoader::wait_idle() {
    if (jobs_) jobs_->wait(counter_);
    collect();
}

bool AsyncLoader::idle() const noexcept { return counter_.done(); }

LoadState AsyncLoader::request(const Uuid& uuid, LoadType type, LoadPriority priority) {
    const Key key{uuid, type};
    if (const auto it = slots_.find(key); it != slots_.end()) {
        Slot& s = it->second;
        if (s.state == LoadState::Queued || s.state == LoadState::Loading || s.state == LoadState::Decoded)
            s.priority = std::max(s.priority, priority);
        return s.state;
    }
    Slot& s = slots_[key];
    s.priority = priority;
    s.order = next_order_++;
    ++totals_.requested;
    dispatch();
    return s.state;
}

LoadState AsyncLoader::state(const Uuid& uuid, LoadType type) const {
    const auto it = slots_.find({uuid, type});
    return it == slots_.end() ? LoadState::None : it->second.state;
}

const Error* AsyncLoader::error(const Uuid& uuid, LoadType type) const {
    const auto it = slots_.find({uuid, type});
    return it != slots_.end() && it->second.state == LoadState::Failed ? &it->second.error : nullptr;
}

void AsyncLoader::finish(Slot& slot, Result<LoadPayload> result) {
    slot.job.reset();
    if (result) {
        slot.payload = std::make_unique<LoadPayload>(std::move(result).value());
        slot.bytes = upload_bytes(*slot.payload);
        slot.state = LoadState::Decoded;
        ++totals_.completed;
    } else {
        slot.error = std::move(result).error();
        slot.state = LoadState::Failed;
        ++totals_.failed;
    }
}

void AsyncLoader::start(const Key& key, Slot& slot) {
    if (!jobs_) {
        finish(slot, fetch_(key.uuid, key.type));
        return;
    }
    auto job = std::make_shared<Job>();
    job->uuid = key.uuid;
    job->type = key.type;
    slot.job = job;
    slot.state = LoadState::Loading;
    ++running_;
    // The job holds its own reference: a slot forgotten mid-decode cannot free it under the worker.
    jobs_->submit(
        [job, fetch = &fetch_] {
            VICTUS_PROFILE_SCOPE("AsyncLoader::fetch");
            job->result.emplace((*fetch)(job->uuid, job->type));
            job->done.store(true, std::memory_order_release);
        },
        &counter_, jobs::Priority::Background);
}

void AsyncLoader::collect() {
    for (auto& [key, slot] : slots_) {
        if (slot.state != LoadState::Loading || !slot.job->done.load(std::memory_order_acquire)) continue;
        --running_;
        finish(slot, std::move(*slot.job->result));
    }
    const auto before = orphans_.size();
    std::erase_if(orphans_, [](const std::shared_ptr<Job>& j) { return j->done.load(std::memory_order_acquire); });
    running_ -= static_cast<std::uint32_t>(before - orphans_.size());
}

void AsyncLoader::dispatch() {
    const std::uint32_t limit = std::max(settings_.max_in_flight, 1u);
    while (!jobs_ || running_ < limit) {
        Slot* best = nullptr;
        const Key* best_key = nullptr;
        for (auto& [key, slot] : slots_) {
            if (slot.state != LoadState::Queued) continue;
            if (!best || slot.priority > best->priority || (slot.priority == best->priority && slot.order < best->order)) {
                best = &slot;
                best_key = &key;
            }
        }
        if (!best) return;
        start(*best_key, *best);
    }
}

std::vector<AsyncLoader::Completed> AsyncLoader::take(std::size_t byte_budget) {
    collect();
    dispatch();
    std::vector<std::pair<const Key*, Slot*>> ready;
    for (auto& [key, slot] : slots_)
        if (slot.state == LoadState::Decoded) ready.emplace_back(&key, &slot);
    std::sort(ready.begin(), ready.end(), [](const auto& a, const auto& b) {
        if (a.second->priority != b.second->priority) return a.second->priority > b.second->priority;
        return a.second->order < b.second->order;
    });
    std::vector<Completed> out;
    std::size_t spent = 0;
    for (auto& [key, slot] : ready) {
        if (!out.empty() && spent + slot->bytes > byte_budget) break;
        spent += slot->bytes;
        out.push_back({key->uuid, key->type, slot->priority, std::move(*slot->payload), slot->bytes});
        slot->payload.reset();
        slot->bytes = 0;
        slot->state = LoadState::Taken;
        ++totals_.taken;
    }
    return out;
}

void AsyncLoader::forget(const Uuid& uuid) {
    for (const LoadType type : {LoadType::Texture, LoadType::Model}) {
        const auto it = slots_.find({uuid, type});
        if (it == slots_.end()) continue;
        if (it->second.state == LoadState::Loading) orphans_.push_back(std::move(it->second.job));
        if (it->second.state == LoadState::Queued || it->second.state == LoadState::Loading) ++totals_.cancelled;
        slots_.erase(it);
    }
}

void AsyncLoader::forget_all() {
    for (auto& [key, slot] : slots_) {
        if (slot.state == LoadState::Loading) orphans_.push_back(std::move(slot.job));
        if (slot.state == LoadState::Queued || slot.state == LoadState::Loading) ++totals_.cancelled;
    }
    slots_.clear();
}

LoaderStats AsyncLoader::stats() const {
    LoaderStats s = totals_;
    for (const auto& [key, slot] : slots_) {
        if (slot.state == LoadState::Queued) ++s.queued;
        else if (slot.state == LoadState::Loading) ++s.loading;
        else if (slot.state == LoadState::Decoded) {
            ++s.decoded;
            s.decoded_bytes += slot.bytes;
        }
    }
    return s;
}

} // namespace victus::assets
