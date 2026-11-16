// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace victus::memory {

// Process-wide allocation counters. Populated only when the executable links
// victus_alloc_tracking (operator new/delete override); otherwise all zero.
struct AllocationStats {
    std::uint64_t allocations{0};
    std::uint64_t frees{0};
    std::uint64_t bytes_allocated{0};
    std::int64_t live_bytes{0};
    std::int64_t peak_live_bytes{0};
};
[[nodiscard]] AllocationStats allocation_stats() noexcept;
[[nodiscard]] bool allocation_tracking_enabled() noexcept;

namespace detail {
void record_alloc(std::size_t bytes) noexcept;
void record_free(std::size_t bytes) noexcept;
void mark_tracking_enabled() noexcept;
} // namespace detail

[[nodiscard]] constexpr std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

// Bump allocator with chunk growth. reset() reuses the first chunk and frees the rest,
// making it suitable for per-frame and per-job scratch memory. Not thread-safe.
class LinearArena {
public:
    explicit LinearArena(std::size_t chunk_bytes = 64 * 1024);
    ~LinearArena();
    LinearArena(const LinearArena&) = delete;
    LinearArena& operator=(const LinearArena&) = delete;

    [[nodiscard]] void* allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t));
    template <typename T, typename... Args>
    [[nodiscard]] T* create(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>, "arena objects are never destroyed individually");
        return new (allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
    }
    template <typename T>
    [[nodiscard]] T* allocate_array(std::size_t count) {
        static_assert(std::is_trivially_destructible_v<T>);
        return static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
    }
    void reset() noexcept;
    [[nodiscard]] std::size_t bytes_used() const noexcept { return used_total_; }
    [[nodiscard]] std::size_t bytes_reserved() const noexcept;
    [[nodiscard]] std::size_t chunk_count() const noexcept { return chunks_.size(); }

private:
    struct Chunk {
        std::byte* data;
        std::size_t size;
    };
    std::vector<Chunk> chunks_;
    std::size_t chunk_bytes_;
    std::size_t offset_{0};
    std::size_t used_total_{0};
};

// Fixed-type object pool with stable addresses and O(1) create/destroy.
template <typename T, std::size_t BlockCount = 256>
class Pool {
public:
    Pool() = default;
    ~Pool() {
        // Objects still alive are the caller's leak; storage is reclaimed regardless.
        for (auto* block : blocks_) ::operator delete(block, std::align_val_t{alignof(Slot)});
    }
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    template <typename... Args>
    [[nodiscard]] T* create(Args&&... args) {
        if (!free_) grow();
        Slot* slot = free_;
        free_ = slot->next;
        ++live_;
        return new (slot->storage) T(std::forward<Args>(args)...);
    }
    void destroy(T* object) noexcept {
        if (!object) return;
        object->~T();
        auto* slot = reinterpret_cast<Slot*>(object);
        slot->next = free_;
        free_ = slot;
        --live_;
    }
    [[nodiscard]] std::size_t live() const noexcept { return live_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return blocks_.size() * BlockCount; }

private:
    union Slot {
        Slot* next;
        alignas(T) std::byte storage[sizeof(T)];
    };
    void grow() {
        auto* block = static_cast<Slot*>(::operator new(sizeof(Slot) * BlockCount, std::align_val_t{alignof(Slot)}));
        blocks_.push_back(block);
        for (std::size_t i = BlockCount; i-- > 0;) {
            block[i].next = free_;
            free_ = &block[i];
        }
    }
    std::vector<Slot*> blocks_;
    Slot* free_{nullptr};
    std::size_t live_{0};
};

} // namespace victus::memory
