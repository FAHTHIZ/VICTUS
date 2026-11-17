// SPDX-License-Identifier: Apache-2.0
#include "victus/render/irradiance_bake.hpp"

#include "victus/core/profiler.hpp"

#include <algorithm>
#include <cmath>

namespace victus::render {

void IrradianceBaker::reset() noexcept {
    pass_ = 0;
    next_ = 0;
    baked_ = 0;
    done_ = false;
}

float IrradianceBaker::progress(const IrradianceVolume& volume, const IrradianceBakeSettings& settings) const noexcept {
    if (done_) return 1.0f;
    const std::uint32_t passes = std::clamp(settings.bounces, 1u, 4u);
    const std::uint32_t total = volume.grid().probe_count() * passes;
    const std::uint32_t finished = pass_ * volume.grid().probe_count() + next_;
    return total ? std::min(static_cast<float>(finished) / static_cast<float>(total), 1.0f) : 1.0f;
}

Status IrradianceBaker::step(GpuContext& ctx, SceneRenderer& renderer, IrradianceVolume& volume, const CaptureFill& fill,
                             const IrradianceBakeSettings& settings, std::uint32_t max_probes) {
    VICTUS_PROFILE_SCOPE("IrradianceBaker::step");
    if (done_) return {};
    const std::uint32_t passes = std::clamp(settings.bounces, 1u, 4u);
    const std::uint32_t face = std::clamp(settings.face_size, 16u, 128u);
    const std::uint32_t count = volume.grid().probe_count();
    std::uint32_t budget = max_probes == 0 ? count * passes : max_probes;
    while (budget > 0 && !done_) {
        const bool bounce = pass_ > 0;
        const CaptureFill with_volume = [&](SceneRenderer& r, const Camera3D& cam) {
            fill(r, cam);
            // Later passes see the probes baked so far: light arriving through them is one bounce more.
            if (bounce) r.set_irradiance_volume(&volume, settings.intensity, settings.normal_offset);
            else r.set_irradiance_volume(nullptr);
        };
        auto img = capture_equirect(ctx, renderer, volume.grid().probe_position(next_), face, with_volume, settings.z_near,
                                    settings.z_far);
        if (!img) return std::move(img).error();
        volume.set_probe(next_, SphericalHarmonics9::project(*img));
        ++baked_;
        --budget;
        if (++next_ >= count) {
            next_ = 0;
            VICTUS_TRY_STATUS(volume.upload()); // the next pass reads this one
            if (++pass_ >= passes) done_ = true;
        }
    }
    return volume.upload();
}

Status IrradianceRefresher::step(GpuContext& ctx, SceneRenderer& renderer, IrradianceVolume& volume, const CaptureFill& fill,
                                 const IrradianceBakeSettings& settings, std::uint32_t max_probes, float blend) {
    VICTUS_PROFILE_SCOPE("IrradianceRefresher::step");
    const std::uint32_t count = volume.grid().probe_count();
    if (count == 0) return {};
    const std::uint32_t face = std::clamp(settings.face_size, 16u, 128u);
    const float w = std::isfinite(blend) ? std::clamp(blend, 0.01f, 1.0f) : 0.5f;
    const float feedback = std::isfinite(settings.intensity) ? std::clamp(settings.intensity, 0.0f, 1.0f) : 1.0f;
    const bool use_volume = volume.valid_count() > 0 && volume.texture();
    const CaptureFill with_volume = [&](SceneRenderer& r, const Camera3D& cam) {
        fill(r, cam);
        r.set_irradiance_volume(use_volume ? &volume : nullptr, feedback, settings.normal_offset);
    };
    std::uint32_t budget = max_probes == 0 ? count : std::min(max_probes, count);
    next_ %= count;
    while (budget-- > 0) {
        auto img = capture_equirect(ctx, renderer, volume.grid().probe_position(next_), face, with_volume, settings.z_near,
                                    settings.z_far);
        if (!img) return std::move(img).error();
        const SphericalHarmonics9 fresh = SphericalHarmonics9::project(*img);
        if (volume.probe_valid(next_)) {
            SphericalHarmonics9 mixed = volume.probe(next_);
            for (std::size_t k = 0; k < mixed.coefficients.size(); ++k)
                mixed.coefficients[k] = mixed.coefficients[k] + (fresh.coefficients[k] - mixed.coefficients[k]) * w;
            volume.set_probe(next_, mixed);
        } else {
            volume.set_probe(next_, fresh);
        }
        ++refreshed_;
        next_ = (next_ + 1) % count;
    }
    return volume.upload();
}

Status bake_irradiance_volume(GpuContext& ctx, SceneRenderer& renderer, IrradianceVolume& volume, const CaptureFill& fill,
                              const IrradianceBakeSettings& settings) {
    IrradianceBaker baker;
    return baker.step(ctx, renderer, volume, fill, settings, 0);
}

} // namespace victus::render
