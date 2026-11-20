// SPDX-License-Identifier: Apache-2.0
// Generational slot map behind the RHI handles (shared by the SDL_GPU and GLES backends).
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace victus::rhi::detail {

template <typename T>
struct SlotMap {
    struct Entry {
        T value{};
        std::uint32_t generation{0};
        bool alive{false};
    };
    std::vector<Entry> entries;
    std::vector<std::uint32_t> free_list;
    std::uint32_t live{0};

    template <typename H>
    H insert(T value) {
        std::uint32_t idx;
        if (!free_list.empty()) {
            idx = free_list.back();
            free_list.pop_back();
        } else {
            idx = static_cast<std::uint32_t>(entries.size());
            entries.push_back({});
        }
        Entry& e = entries[idx];
        e.value = std::move(value);
        e.generation = e.generation + 1 == 0 ? 1 : e.generation + 1;
        e.alive = true;
        ++live;
        return H{idx, e.generation};
    }
    template <typename H>
    T* get(H h) {
        if (h.index >= entries.size()) return nullptr;
        Entry& e = entries[h.index];
        return (e.alive && e.generation == h.generation) ? &e.value : nullptr;
    }
    template <typename H>
    bool remove(H h, T& out) {
        T* v = get(h);
        if (!v) return false;
        out = std::move(*v);
        entries[h.index].alive = false;
        free_list.push_back(h.index);
        --live;
        return true;
    }
};

} // namespace victus::rhi::detail
