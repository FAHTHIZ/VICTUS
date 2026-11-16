// SPDX-License-Identifier: Apache-2.0
#include "victus/core/hash.hpp"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <random>

namespace victus {

std::uint64_t fnv1a64(std::span<const std::uint8_t> bytes, std::uint64_t seed) noexcept {
    std::uint64_t h = seed;
    for (const auto b : bytes) {
        h ^= b;
        h *= 0x100000001b3ull;
    }
    return h;
}

std::string Uuid::to_string() const {
    char buf[37];
    std::snprintf(buf, sizeof buf, "%08x-%04x-%04x-%04x-%012llx", static_cast<unsigned>(hi >> 32),
                  static_cast<unsigned>((hi >> 16) & 0xffff), static_cast<unsigned>(hi & 0xffff),
                  static_cast<unsigned>(lo >> 48), static_cast<unsigned long long>(lo & 0xffffffffffffull));
    return buf;
}

bool Uuid::parse(std::string_view text, Uuid& out) noexcept {
    if (text.size() != 36) return false;
    std::uint64_t parts[2]{0, 0};
    int nibbles = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
            continue;
        }
        std::uint64_t v;
        if (c >= '0' && c <= '9') v = static_cast<std::uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v = static_cast<std::uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = static_cast<std::uint64_t>(c - 'A' + 10);
        else return false;
        auto& p = parts[nibbles / 16];
        p = (p << 4) | v;
        ++nibbles;
    }
    out = Uuid{parts[0], parts[1]};
    return nibbles == 32;
}

Uuid Uuid::generate() {
    static std::mutex mutex;
    static std::mt19937_64 engine([] {
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), rd(),
                          static_cast<unsigned>(std::chrono::high_resolution_clock::now().time_since_epoch().count())};
        return std::mt19937_64(seq);
    }());
    std::lock_guard lock(mutex);
    Uuid u{engine(), engine()};
    // RFC 9562 version 4, variant 1.
    u.hi = (u.hi & ~0xf000ull) | 0x4000ull;
    u.lo = (u.lo & ~(0xc000ull << 48)) | (0x8000ull << 48);
    return u;
}

} // namespace victus
