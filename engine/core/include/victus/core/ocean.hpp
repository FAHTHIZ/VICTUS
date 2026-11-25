// SPDX-License-Identifier: Apache-2.0
// FFT ocean (ADR-0261): Tessendorf's statistical wave model ("Simulating Ocean Water", 2001).
// A Phillips spectrum is sampled once on an N x N grid of wave vectors; each evaluation advances
// every wave by its deep-water dispersion and inverse-FFTs height, choppy horizontal displacement,
// slopes and the displacement derivatives into a periodic tile. Everything runs on the CPU in
// float with a fixed seed, so the server (buoyancy), clients and the renderer see the same sea.
// Lives in core because headless servers need it for buoyancy without the renderer.
#pragma once

#include "victus/core/math.hpp"

#include <complex>
#include <cstdint>
#include <vector>

namespace victus::ocean {

struct SpectrumDesc {
    std::uint32_t resolution{64};  // waves per side of the tile; power of two, 16..256
    float tile_size{100.0f};       // metres; the surface repeats with this period along X and Z
    float wave_height{1.0f};       // significant wave height (4 x standard deviation), metres
    float peak_wavelength{20.0f};  // metres; sets the wind speed (Phillips L = V^2 / g)
    float direction{0.0f};         // wind direction, radians from +X towards +Z
    float choppiness{1.0f};        // horizontal displacement factor (Tessendorf lambda, 0..2)
    float speed{1.0f};             // multiplier on deep-water dispersion
    std::uint32_t seed{1};
};
// Clamps and fixes non-finite values (resolution rounded down to a power of two).
[[nodiscard]] SpectrumDesc sanitize(SpectrumDesc d) noexcept;
[[nodiscard]] bool operator==(const SpectrumDesc& a, const SpectrumDesc& b) noexcept;

// One evaluated tile. Texel (i, j) is the rest point (i, j) * tile_size / n in world X and Z
// (world origin at texel 0, so neighbouring volumes with the same spectrum line up).
struct Field {
    std::uint32_t n{0};
    float tile_size{0};
    double time{0};
    std::uint64_t revision{0}; // unique per evaluation (process-wide), for upload caches
    // xyz displacement of the rest point (x, z horizontal, y height); w unused.
    std::vector<math::Vec4> displacement;
    // xyz unit normal of the displaced surface; w Jacobian of the horizontal displacement
    // (< 1 where the surface compresses: crests, foam; < 0 where it folds over).
    std::vector<math::Vec4> normal;
    float max_height{0};     // largest |height| in this evaluation
    float max_horizontal{0}; // largest horizontal displacement length in this evaluation
};

struct Sample {
    float height{0}; // above the rest level
    math::Vec3 normal{0, 1, 0};
    float jacobian{1};
};

class Spectrum {
public:
    explicit Spectrum(const SpectrumDesc& desc);
    [[nodiscard]] const SpectrumDesc& desc() const noexcept { return desc_; }
    // Fills `out` with the surface at `time` seconds (phases computed in double precision).
    void evaluate(double time, Field& out) const;
    [[nodiscard]] Field evaluate(double time) const {
        Field f;
        evaluate(time, f);
        return f;
    }

private:
    SpectrumDesc desc_;
    std::vector<std::complex<float>> h0_;       // h0(k)
    std::vector<std::complex<float>> h0_conj_;  // conj(h0(-k))
    std::vector<double> omega_;                 // rad/s
    std::vector<math::Vec2> k_;                 // wave vectors (rad/m)
    std::vector<std::complex<float>> twiddles_; // inverse FFT factors for this resolution
};

// Bilinear, periodic lookups at world `xz`.
[[nodiscard]] math::Vec4 displacement_at(const Field& f, math::Vec2 xz) noexcept;
[[nodiscard]] math::Vec4 normal_at(const Field& f, math::Vec2 xz) noexcept;
// The surface above world `xz`: finds the rest point whose horizontal displacement lands on xz
// (fixed-point iteration, as for the Gerstner waves) and returns its height and normal.
[[nodiscard]] Sample sample(const Field& f, math::Vec2 xz) noexcept;

// In-place radix-2 complex FFT of length data.size() (a power of two); `inverse` uses e^{+i}
// and does not scale. Exposed for tests.
void fft(std::vector<std::complex<float>>& data, bool inverse);

} // namespace victus::ocean
