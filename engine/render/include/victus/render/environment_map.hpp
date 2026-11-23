// SPDX-License-Identifier: Apache-2.0
// Image-based lighting: HDR environment maps baked on the GPU into a GGX-prefiltered cubemap
// (specular reflections, one roughness level per mip) plus 9-coefficient spherical harmonics
// for diffuse irradiance. Baking is a handful of fragment passes (no compute), so it runs at
// load time on every SDL_GPU target including tile-based mobile GPUs; sampling costs one
// cubemap fetch and nine multiply-adds per pixel.
#pragma once
#include "victus/render/gpu_context.hpp"
#include "victus/render/image.hpp"

#include <array>
#include <memory>

namespace victus::render {

// Equirectangular (latitude-longitude) mapping used by every VICTUS environment map:
// u = 0.5 looks down -Z (the default camera forward), u increases towards +X, v = 0 is +Y (up).
[[nodiscard]] math::Vec3 equirect_direction(float u, float v) noexcept;
[[nodiscard]] math::Vec2 equirect_uv(math::Vec3 direction) noexcept;

// Order-2 (9 coefficient) spherical harmonics of the cosine-convolved environment, scaled so
// that `diffuse(n)` is irradiance / pi: the outgoing radiance of a white Lambertian surface
// with normal n (Ramamoorthi & Hanrahan 2001).
struct SphericalHarmonics9 {
    std::array<math::Vec3, 9> coefficients{};
    [[nodiscard]] static SphericalHarmonics9 project(const ImageF& equirect);
    [[nodiscard]] math::Vec3 diffuse(math::Vec3 n) const noexcept;
    // Average radiance over the sphere (the DC term without the convolution).
    [[nodiscard]] math::Vec3 average() const noexcept;
};

// Sun extraction (ADR-0236). Captured HDR skies (Poly Haven, ambientCG...) hold the sun as a few
// texels tens of thousands of times brighter than the sky. Projected into SH9 irradiance and
// GGX-prefiltered into the reflection cube, it becomes a second, blurred sun from the photo's sun
// direction that lights every shadow the scene's own directional light casts, which is why HDR-lit
// scenes look flat and milky. `extract_sun` clamps those texels to the sky's own brightness and
// reports what it removed, so the scene's DirectionalLight carries the sun alone (Frostbite's
// "Moving Frostbite to PBR" 4.9.4, Filament's IBL notes; Unreal's HDRI workflow pairs a
// sun-free sky with a directional light in the same way).
struct ExtractedSun {
    bool found{false};
    math::Vec3 direction{0, 1, 0};  // unit vector towards the sun, equirect_direction convention
    math::Vec3 irradiance{0, 0, 0}; // linear rgb irradiance removed, at normal incidence
    float threshold{0.0f};          // luminance the sun texels were clamped to
    std::uint32_t texels{0};        // texels clamped
};
// Finds a sun (a peak at least `min_peak_ratio` times the median sky luminance) and clamps every
// texel brighter than the threshold to it, keeping its colour. Images without such a peak
// (overcast skies, interiors, synthetic gradients) are left unchanged and `found` is false.
ExtractedSun extract_sun(ImageF& equirect, float min_peak_ratio = 500.0f);

struct EnvironmentBakeSettings {
    std::uint32_t cube_size{256}; // face resolution of mip 0 (16..2048, rounded to a power of two)
    std::uint32_t samples{64};    // GGX samples per prefiltered texel (8..1024)
    // Low tier: 128 / 32 keeps the bake cheap on entry-level phones; reflections stay plausible.
    [[nodiscard]] static EnvironmentBakeSettings low() noexcept { return {128, 32}; }
};

class EnvironmentMap {
public:
    // Bakes `equirect` (linear HDR radiance). `sh` defaults to projecting the image on the CPU.
    [[nodiscard]] static Result<std::unique_ptr<EnvironmentMap>> create(GpuContext& ctx, const ImageF& equirect,
                                                                        const EnvironmentBakeSettings& settings = {});
    [[nodiscard]] static Result<std::unique_ptr<EnvironmentMap>> create(GpuContext& ctx, const ImageF& equirect,
                                                                        const SphericalHarmonics9& sh,
                                                                        const EnvironmentBakeSettings& settings = {});
    ~EnvironmentMap();
    EnvironmentMap(const EnvironmentMap&) = delete;
    EnvironmentMap& operator=(const EnvironmentMap&) = delete;

    // RGBA16F cubemap: mip 0 is the sharp environment, mip i is GGX-prefiltered for
    // roughness i / (mip_count - 1).
    [[nodiscard]] rhi::TextureHandle cube() const noexcept { return cube_; }
    [[nodiscard]] std::uint32_t mip_count() const noexcept { return mips_; }
    [[nodiscard]] std::uint32_t size() const noexcept { return size_; }
    [[nodiscard]] const SphericalHarmonics9& sh() const noexcept { return sh_; }

private:
    explicit EnvironmentMap(GpuContext& ctx) : ctx_(ctx) {}
    GpuContext& ctx_;
    rhi::TextureHandle cube_;
    std::uint32_t mips_{1}, size_{0};
    SphericalHarmonics9 sh_;
};

} // namespace victus::render
