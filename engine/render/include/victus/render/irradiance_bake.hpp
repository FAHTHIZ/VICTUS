// SPDX-License-Identifier: Apache-2.0
// Progressive irradiance volume baking (ADR-0056): each probe renders a small cube capture of the
// lit scene (reflection_capture.hpp) and projects it into SH9. The first pass sees direct light and
// the environment; later passes render with the volume itself applied, adding one bounce of
// indirect light per pass. Work is spread over frames with a probe budget per step.
#pragma once
#include "victus/render/irradiance_volume.hpp"
#include "victus/render/reflection_capture.hpp"

namespace victus::render {

struct IrradianceBakeSettings {
    std::uint32_t face_size{16}; // capture face size (16..128): irradiance is low frequency
    std::uint32_t bounces{2};    // passes over all probes (1..4); pass n adds the n-th bounce
    float intensity{1.0f};       // applied while rendering later passes (as at run time)
    float normal_offset{0.0f};
    float z_near{0.05f};
    float z_far{500.0f};
};

class IrradianceBaker {
public:
    // Starts again from the first probe of the first pass.
    void reset() noexcept;
    [[nodiscard]] bool done() const noexcept { return done_; }
    [[nodiscard]] std::uint32_t pass() const noexcept { return pass_; }
    [[nodiscard]] std::uint32_t probes_baked() const noexcept { return baked_; }
    // Fraction of the total work (all passes) finished, 0..1.
    [[nodiscard]] float progress(const IrradianceVolume& volume, const IrradianceBakeSettings& settings) const noexcept;
    // Bakes up to `max_probes` probes (0 = all remaining) and uploads the volume.
    [[nodiscard]] Status step(GpuContext& ctx, SceneRenderer& renderer, IrradianceVolume& volume, const CaptureFill& fill,
                              const IrradianceBakeSettings& settings, std::uint32_t max_probes);

private:
    std::uint32_t pass_{0}, next_{0}, baked_{0};
    bool done_{false};
};

// Dynamic GI (ADR-0057): re-captures probes round-robin with the volume itself applied, blending
// each new capture into the stored probe. Converges to multi-bounce lighting over refresh cycles
// and follows light changes (sun, moving lights, emissive edits) with a lag of about
// probe_count / probes-per-frame frames divided by the blend factor.
class IrradianceRefresher {
public:
    void reset() noexcept { next_ = 0; }
    [[nodiscard]] std::uint32_t next_probe() const noexcept { return next_; }
    [[nodiscard]] std::uint64_t probes_refreshed() const noexcept { return refreshed_; }
    // Refreshes up to `max_probes` probes (0 = one full cycle). `blend` in (0, 1]: weight of the new
    // capture (1 = replace). Feedback renders clamp the volume intensity to 1 so it cannot diverge.
    [[nodiscard]] Status step(GpuContext& ctx, SceneRenderer& renderer, IrradianceVolume& volume, const CaptureFill& fill,
                              const IrradianceBakeSettings& settings, std::uint32_t max_probes, float blend = 0.5f);

private:
    std::uint32_t next_{0};
    std::uint64_t refreshed_{0};
};

// Bakes every pass of every probe at once (tools, tests, offline bakes).
[[nodiscard]] Status bake_irradiance_volume(GpuContext& ctx, SceneRenderer& renderer, IrradianceVolume& volume,
                                            const CaptureFill& fill, const IrradianceBakeSettings& settings = {});

} // namespace victus::render
