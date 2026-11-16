// SPDX-License-Identifier: Apache-2.0
#include "victus/core/memory.hpp"

#include <algorithm>
#include <cstdlib>

namespace victus::memory {

namespace {
std::atomic<std::uint64_t> g_allocs{0};
std::atomic<std::uint64_t> g_frees{0};
std::atomic<std::uint64_t> g_bytes{0};
std::atomic<std::int64_t> g_live{0};
std::atomic<std::int64_t> g_peak{0};
std::atomic<bool> g_enabled{false};
} // namespace

namespace detail {
void record_alloc(std::size_t bytes) noexcept {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    g_bytes.fetch_add(bytes, std::memory_order_relaxed);
    const auto live = g_live.fetch_add(static_cast<std::int64_t>(bytes), std::memory_order_relaxed) + static_cast<std::int64_t>(bytes);
    auto peak = g_peak.load(std::memory_order_relaxed);
    while (live > peak && !g_peak.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {
    }
}
void record_free(std::size_t bytes) noexcept {
    g_frees.fetch_add(1, std::memory_order_relaxed);
    g_live.fetch_sub(static_cast<std::int64_t>(bytes), std::memory_order_relaxed);
}
void mark_tracking_enabled() noexcept { g_enabled.store(true, std::memory_order_relaxed); }
} // namespace detail

AllocationStats allocation_stats() noexcept {
    return {g_allocs.load(std::memory_order_relaxed), g_frees.load(std::memory_order_relaxed),
            g_bytes.load(std::memory_order_relaxed), g_live.load(std::memory_order_relaxed),
            g_peak.load(std::memory_order_relaxed)};
}
bool allocation_tracking_enabled() noexcept { return g_enabled.load(std::memory_order_relaxed); }

LinearArena::LinearArena(std::size_t chunk_bytes) : chunk_bytes_(std::max<std::size_t>(chunk_bytes, 256)) {}

LinearArena::~LinearArena() {
    for (auto& c : chunks_) ::operator delete(c.data, std::align_val_t{64});
}

void* LinearArena::allocate(std::size_t bytes, std::size_t alignment) {
    if (alignment > 64) alignment = 64;
    if (!chunks_.empty()) {
        Chunk& c = chunks_.back();
        const std::size_t start = align_up(offset_, alignment);
        if (start + bytes <= c.size) {
            offset_ = start + bytes;
            used_total_ += bytes;
            return c.data + start;
        }
    }
    const std::size_t size = std::max(chunk_bytes_, align_up(bytes, 64));
    auto* data = static_cast<std::byte*>(::operator new(size, std::align_val_t{64}));
    chunks_.push_back({data, size});
    offset_ = bytes;
    used_total_ += bytes;
    return data;
}

void LinearArena::reset() noexcept {
    while (chunks_.size() > 1) {
        ::operator delete(chunks_.back().data, std::align_val_t{64});
        chunks_.pop_back();
    }
    offset_ = 0;
    used_total_ = 0;
}

std::size_t LinearArena::bytes_reserved() const noexcept {
    std::size_t total = 0;
    for (const auto& c : chunks_) total += c.size;
    return total;
}

} // namespace victus::memory
