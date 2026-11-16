// SPDX-License-Identifier: Apache-2.0
// Opt-in global allocation tracking. Link the victus_alloc_tracking object library
// into an executable to count every heap allocation (benchmarks, runtime diagnostics).
#include "victus/core/memory.hpp"

#include <cstdlib>
#include <new>

#if defined(_WIN32)
#include <malloc.h>
#define VICTUS_MSIZE(p) _msize(p)
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#define VICTUS_MSIZE(p) malloc_size(p)
#else
#include <malloc.h>
#define VICTUS_MSIZE(p) malloc_usable_size(p)
#endif

namespace {
struct Enable {
    Enable() { victus::memory::detail::mark_tracking_enabled(); }
} g_enable;

void* tracked_alloc(std::size_t size) {
    void* p = std::malloc(size ? size : 1);
    if (!p) throw std::bad_alloc();
    victus::memory::detail::record_alloc(VICTUS_MSIZE(p));
    return p;
}
void tracked_free(void* p) noexcept {
    if (!p) return;
    victus::memory::detail::record_free(VICTUS_MSIZE(p));
    std::free(p);
}
} // namespace

void* operator new(std::size_t size) { return tracked_alloc(size); }
void* operator new[](std::size_t size) { return tracked_alloc(size); }
void operator delete(void* p) noexcept { tracked_free(p); }
void operator delete[](void* p) noexcept { tracked_free(p); }
void operator delete(void* p, std::size_t) noexcept { tracked_free(p); }
void operator delete[](void* p, std::size_t) noexcept { tracked_free(p); }
// Aligned forms are left to the C++ runtime (not counted) to avoid platform-specific
// aligned-allocation pairing rules; engine aligned allocations go through arenas.
