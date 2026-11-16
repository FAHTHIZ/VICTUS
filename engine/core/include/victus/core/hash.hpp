// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace victus {

// 64-bit FNV-1a. Stable across platforms; used for content keys and names, not security.
[[nodiscard]] constexpr std::uint64_t fnv1a64(std::string_view s, std::uint64_t seed = 0xcbf29ce484222325ull) noexcept {
    std::uint64_t h = seed;
    for (const char c : s) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 0x100000001b3ull;
    }
    return h;
}
[[nodiscard]] std::uint64_t fnv1a64(std::span<const std::uint8_t> bytes, std::uint64_t seed = 0xcbf29ce484222325ull) noexcept;

// Standard base64 (RFC 4648 section 4, '=' padding), e.g. for glTF data URIs.
[[nodiscard]] std::string base64_encode(std::span<const std::uint8_t> bytes);

// Deterministic PCG32 random stream (O'Neill, Apache-2.0 reference algorithm).
// Identical output on every platform for a given seed/stream: required for replays,
// procedural generation and lockstep simulation.
class Random {
public:
    explicit Random(std::uint64_t seed = 0x853c49e6748fea9bull, std::uint64_t stream = 0xda3e39cb94b95bdbull) noexcept {
        reseed(seed, stream);
    }
    void reseed(std::uint64_t seed, std::uint64_t stream = 0xda3e39cb94b95bdbull) noexcept {
        state_ = 0;
        inc_ = (stream << 1u) | 1u;
        next_u32();
        state_ += seed;
        next_u32();
    }
    std::uint32_t next_u32() noexcept {
        const std::uint64_t old = state_;
        state_ = old * 6364136223846793005ull + inc_;
        const auto xorshifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
        const auto rot = static_cast<std::uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
    }
    std::uint64_t next_u64() noexcept { return (static_cast<std::uint64_t>(next_u32()) << 32) | next_u32(); }
    // Unbiased integer in [0, bound).
    std::uint32_t uniform(std::uint32_t bound) noexcept {
        if (bound == 0) return 0;
        const std::uint32_t threshold = (~bound + 1u) % bound;
        for (;;) {
            const std::uint32_t r = next_u32();
            if (r >= threshold) return r % bound;
        }
    }
    std::int32_t range(std::int32_t lo, std::int32_t hi) noexcept { // inclusive
        return lo + static_cast<std::int32_t>(uniform(static_cast<std::uint32_t>(hi - lo + 1)));
    }
    // Float in [0,1) built from 24 random bits — exact and portable.
    float next_float() noexcept { return static_cast<float>(next_u32() >> 8) * (1.0f / 16777216.0f); }
    float range_float(float lo, float hi) noexcept { return lo + (hi - lo) * next_float(); }
    [[nodiscard]] std::uint64_t state() const noexcept { return state_; }
    [[nodiscard]] std::uint64_t increment() const noexcept { return inc_; }
    void restore(std::uint64_t state, std::uint64_t inc) noexcept {
        state_ = state;
        inc_ = inc | 1u;
    }

private:
    std::uint64_t state_{0};
    std::uint64_t inc_{0};
};

// 128-bit identifier used for assets, entities in saved scenes and projects.
struct Uuid {
    std::uint64_t hi{0};
    std::uint64_t lo{0};
    [[nodiscard]] bool is_nil() const noexcept { return hi == 0 && lo == 0; }
    [[nodiscard]] std::string to_string() const;
    static bool parse(std::string_view text, Uuid& out) noexcept;
    static Uuid generate();
    friend bool operator==(const Uuid&, const Uuid&) = default;
    friend auto operator<=>(const Uuid&, const Uuid&) = default;
};

struct UuidHash {
    std::size_t operator()(const Uuid& u) const noexcept { return static_cast<std::size_t>(u.hi ^ (u.lo * 0x9e3779b97f4a7c15ull)); }
};

} // namespace victus
