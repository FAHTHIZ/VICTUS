// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/core/engine_clock.hpp"

#include <stdexcept>

using victus::core::EngineClock;
using Clock = EngineClock::Clock;

TEST_CASE("engine clock: first advance only initializes") {
    EngineClock clock;
    CHECK(clock.advance(Clock::time_point{}).steps == 0);
}

TEST_CASE("engine clock: fixed steps accumulate with interpolation") {
    EngineClock clock(0.01, 8);
    clock.reset(Clock::time_point{});
    const auto r = clock.advance(Clock::time_point{} + std::chrono::milliseconds(25));
    CHECK(r.steps == 2);
    CHECK(r.interpolation_alpha == doctest::Approx(0.5));
}

TEST_CASE("engine clock: catch-up is bounded and dropped time reported") {
    EngineClock clock(0.01, 3);
    clock.reset(Clock::time_point{});
    const auto r = clock.advance(Clock::time_point{} + std::chrono::milliseconds(105));
    CHECK(r.steps == 3);
    CHECK(r.dropped_seconds > 0.0);
    CHECK(r.interpolation_alpha >= 0.0);
    CHECK(r.interpolation_alpha < 1.0);
}

TEST_CASE("engine clock: invalid configuration rejected") {
    CHECK_THROWS_AS(EngineClock(0.0, 8), std::invalid_argument);
    CHECK_THROWS_AS(EngineClock(0.01, 0), std::invalid_argument);
}
