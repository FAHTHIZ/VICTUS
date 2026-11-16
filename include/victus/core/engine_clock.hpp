#pragma once
#include <chrono>
#include <cstdint>

namespace victus::core {
struct ClockAdvance {
    std::uint32_t steps{0};
    double interpolation_alpha{0.0};
    double dropped_seconds{0.0};
};

// Accumulates wall time into bounded fixed simulation steps.
class EngineClock final {
public:
    using Clock = std::chrono::steady_clock;
    explicit EngineClock(double fixed_step_seconds = 1.0 / 60.0,
                         std::uint32_t max_steps_per_advance = 8);
    void reset(Clock::time_point now = Clock::now()) noexcept;
    [[nodiscard]] ClockAdvance advance(Clock::time_point now = Clock::now()) noexcept;
    [[nodiscard]] double fixed_step_seconds() const noexcept { return fixed_step_seconds_; }
    [[nodiscard]] double accumulated_seconds() const noexcept { return accumulator_seconds_; }
private:
    double fixed_step_seconds_;
    double accumulator_seconds_{0.0};
    std::uint32_t max_steps_per_advance_;
    Clock::time_point previous_time_{};
    bool initialized_{false};
};
} // namespace victus::core
