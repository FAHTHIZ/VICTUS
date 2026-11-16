#include "victus/core/engine_clock.hpp"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
void first_advance_initializes() {
    using Clock = victus::core::EngineClock::Clock;
    victus::core::EngineClock clock;
    require(clock.advance(Clock::time_point{}).steps == 0, "first advance initializes");
}
void fixed_steps_accumulate() {
    using Clock = victus::core::EngineClock::Clock;
    victus::core::EngineClock clock(0.01, 8);
    clock.reset(Clock::time_point{});
    const auto r = clock.advance(Clock::time_point{} + std::chrono::milliseconds(25));
    require(r.steps == 2, "25ms produces two 10ms steps");
    require(std::abs(r.interpolation_alpha - 0.5) < 1e-9, "5ms remainder alpha");
}
void catch_up_is_bounded() {
    using Clock = victus::core::EngineClock::Clock;
    victus::core::EngineClock clock(0.01, 3);
    clock.reset(Clock::time_point{});
    const auto r = clock.advance(Clock::time_point{} + std::chrono::milliseconds(105));
    require(r.steps == 3, "catch-up is bounded");
    require(r.dropped_seconds > 0.0, "dropped overdue time is reported");
    require(r.interpolation_alpha >= 0.0 && r.interpolation_alpha < 1.0, "alpha is in [0,1)");
}
void invalid_configuration_rejected() {
    bool rejected = false;
    try { victus::core::EngineClock clock(0.0, 8); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "zero step rejected");
}
}
int main() {
    first_advance_initializes();
    fixed_steps_accumulate();
    catch_up_is_bounded();
    invalid_configuration_rejected();
    std::cout << "All VICTUS core tests passed.\n";
    return EXIT_SUCCESS;
}
