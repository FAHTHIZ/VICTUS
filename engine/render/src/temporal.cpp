// SPDX-License-Identifier: Apache-2.0
#include "victus/render/temporal.hpp"

#include <algorithm>
#include <cmath>

namespace victus::render {

float halton(std::uint32_t index, std::uint32_t base) noexcept {
    float f = 1.0f, r = 0.0f;
    while (index > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(index % base);
        index /= base;
    }
    return r;
}

math::Vec2 taa_jitter(std::uint64_t frame, std::uint32_t period) noexcept {
    const auto i = static_cast<std::uint32_t>(frame % std::max(period, 1u)) + 1u; // skip index 0 (= 0, 0)
    return {halton(i, 2) - 0.5f, halton(i, 3) - 0.5f};
}

math::Mat4 jitter_projection(const math::Mat4& proj, math::Vec2 pixels, std::uint32_t width, std::uint32_t height) noexcept {
    const float ox = 2.0f * pixels.x / static_cast<float>(std::max(width, 1u));
    const float oy = 2.0f * pixels.y / static_cast<float>(std::max(height, 1u));
    math::Mat4 m = proj;
    for (int c = 0; c < 4; ++c) { // clip.xy += offset * clip.w
        m[c].x += ox * m[c].w;
        m[c].y += oy * m[c].w;
    }
    return m;
}

namespace {
DynamicResolutionSettings sanitize(DynamicResolutionSettings s) noexcept {
    const auto fin = [](float v, float d) { return std::isfinite(v) ? v : d; };
    s.target_frame_ms = std::max(fin(s.target_frame_ms, 16.667f), 0.1f);
    s.min_scale = std::clamp(fin(s.min_scale, 0.5f), 0.25f, 2.0f);
    s.max_scale = std::clamp(fin(s.max_scale, 1.0f), s.min_scale, 2.0f);
    s.headroom = std::clamp(fin(s.headroom, 0.85f), 0.1f, 1.0f);
    s.smoothing = std::clamp(fin(s.smoothing, 0.1f), 0.01f, 1.0f);
    s.increase_step = std::clamp(fin(s.increase_step, 0.05f), 0.0f, 1.0f);
    return s;
}
} // namespace

DynamicResolution::DynamicResolution(DynamicResolutionSettings settings, float initial_scale) noexcept
    : settings_(sanitize(settings)) {
    reset(initial_scale);
}

void DynamicResolution::reset(float scale) noexcept {
    scale_ = std::clamp(std::isfinite(scale) ? scale : 1.0f, settings_.min_scale, settings_.max_scale);
    average_ = 0.0f;
    good_frames_ = 0;
    cooldown_ = 0;
}

float DynamicResolution::update(float frame_ms) noexcept {
    if (!std::isfinite(frame_ms) || frame_ms <= 0.0f) return scale_;
    frame_ms = std::min(frame_ms, settings_.target_frame_ms * 10.0f); // hitches must not collapse the scale
    average_ = average_ <= 0.0f ? frame_ms : average_ + (frame_ms - average_) * settings_.smoothing;
    if (cooldown_ > 0) --cooldown_;
    const float target = settings_.target_frame_ms;
    if (average_ > target) {
        good_frames_ = 0;
        if (cooldown_ == 0 && scale_ > settings_.min_scale) {
            scale_ = std::max(settings_.min_scale, scale_ * std::sqrt(target / average_) * 0.98f);
            cooldown_ = settings_.decrease_cooldown;
            average_ = target; // the next measurements reflect the new scale
        }
    } else if (average_ < target * settings_.headroom) {
        if (++good_frames_ >= settings_.increase_delay && scale_ < settings_.max_scale) {
            scale_ = std::min(settings_.max_scale, scale_ + settings_.increase_step);
            good_frames_ = 0;
        }
    } else {
        good_frames_ = 0;
    }
    return scale_;
}

} // namespace victus::render
