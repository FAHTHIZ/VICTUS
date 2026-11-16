#include "victus/core/engine_clock.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace victus::core {
EngineClock::EngineClock(const double fixed_step_seconds, const std::uint32_t max_steps_per_advance)
    : fixed_step_seconds_(fixed_step_seconds), max_steps_per_advance_(max_steps_per_advance) {
    if (!std::isfinite(fixed_step_seconds_) || fixed_step_seconds_ <= 0.0)
        throw std::invalid_argument("fixed_step_seconds must be finite and greater than zero");
    if (max_steps_per_advance_ == 0)
        throw std::invalid_argument("max_steps_per_advance must be greater than zero");
}
void EngineClock::reset(const Clock::time_point now) noexcept {
    previous_time_ = now;
    accumulator_seconds_ = 0.0;
    initialized_ = true;
}
ClockAdvance EngineClock::advance(const Clock::time_point now) noexcept {
    if (!initialized_) { reset(now); return {}; }
    const double elapsed = std::chrono::duration<double>(now - previous_time_).count();
    previous_time_ = now;
    if (!std::isfinite(elapsed) || elapsed <= 0.0)
        return {0, accumulator_seconds_ / fixed_step_seconds_, 0.0};

    accumulator_seconds_ += elapsed;
    const auto available = static_cast<std::uint64_t>(std::floor(accumulator_seconds_ / fixed_step_seconds_));
    const auto steps = static_cast<std::uint32_t>(std::min<std::uint64_t>(available, max_steps_per_advance_));
    accumulator_seconds_ -= static_cast<double>(steps) * fixed_step_seconds_;
    double dropped = 0.0;
    if (available > max_steps_per_advance_) {
        dropped = static_cast<double>(available - max_steps_per_advance_) * fixed_step_seconds_;
        accumulator_seconds_ = std::fmod(accumulator_seconds_, fixed_step_seconds_);
    }
    return {steps, accumulator_seconds_ / fixed_step_seconds_, dropped};
}
} // namespace victus::core
