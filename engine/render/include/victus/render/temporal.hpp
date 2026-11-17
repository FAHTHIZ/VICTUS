// SPDX-License-Identifier: Apache-2.0
// Temporal rendering helpers: the sub-pixel jitter sequence used by TAA and a dynamic
// resolution controller that trades internal 3D resolution for frame time.
#pragma once
#include "victus/core/math.hpp"

#include <cstdint>

namespace victus::render {

// Radical inverse of `index` in `base` (Halton low-discrepancy sequence), in [0, 1).
[[nodiscard]] float halton(std::uint32_t index, std::uint32_t base) noexcept;

// TAA jitter for frame `frame` in pixels, within [-0.5, 0.5); `period` samples (Halton 2,3)
// repeat. The mean over a full period is close to zero, so a converged image is not shifted.
[[nodiscard]] math::Vec2 taa_jitter(std::uint64_t frame, std::uint32_t period = 8) noexcept;

// Returns `proj` with its clip-space output shifted by `pixels` at a `width` x `height` target
// (x right, y up in NDC). Depth and w are unchanged.
[[nodiscard]] math::Mat4 jitter_projection(const math::Mat4& proj, math::Vec2 pixels, std::uint32_t width,
                                           std::uint32_t height) noexcept;

struct DynamicResolutionSettings {
    float target_frame_ms{1000.0f / 60.0f};
    float min_scale{0.5f};
    float max_scale{1.0f};
    float headroom{0.85f};          // grow only while the smoothed frame time is below target * headroom
    float smoothing{0.1f};          // exponential moving average factor per frame
    float increase_step{0.05f};     // scale added after `increase_delay` frames of headroom
    std::uint32_t increase_delay{30};
    std::uint32_t decrease_cooldown{8}; // frames between reductions (lets the new scale take effect)
};

// Frame-time driven render scale. GPU cost is roughly proportional to pixel count, so a frame
// that is too slow scales by sqrt(target / measured); recovery is slow and stepwise to avoid
// oscillation. Feed it the real frame time once per frame.
class DynamicResolution {
public:
    explicit DynamicResolution(DynamicResolutionSettings settings = {}, float initial_scale = 1.0f) noexcept;
    // Returns the render scale for the next frame.
    float update(float frame_ms) noexcept;
    [[nodiscard]] float scale() const noexcept { return scale_; }
    [[nodiscard]] float smoothed_frame_ms() const noexcept { return average_; }
    [[nodiscard]] const DynamicResolutionSettings& settings() const noexcept { return settings_; }
    void reset(float scale) noexcept;

private:
    DynamicResolutionSettings settings_;
    float scale_{1.0f};
    float average_{0.0f};
    std::uint32_t good_frames_{0};
    std::uint32_t cooldown_{0};
};

} // namespace victus::render
