// SPDX-License-Identifier: Apache-2.0
// FFT ocean (ADR-0261). Notation follows Tessendorf 2001: h(x, t) = sum_k h~(k, t) e^{i k.x},
// h~(k, t) = h0(k) e^{i w t} + conj(h0(-k)) e^{-i w t}, w = sqrt(g |k|), Phillips spectrum
// P(k) = exp(-1 / (k L)^2) / k^4 |k^ . w^|^2 exp(-k^2 l^2). Eight real fields (height, the two
// horizontal displacements, the two slopes and three displacement derivatives) are packed in
// pairs into four complex inverse FFTs.
#include "victus/core/ocean.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <numbers>

namespace victus::ocean {

namespace {

constexpr float kGravity = 9.81f;
constexpr double kTwoPi = 2.0 * std::numbers::pi;
std::atomic<std::uint64_t> g_revision{0};

float finite_or(float v, float fallback) { return std::isfinite(v) ? v : fallback; }

// SplitMix64 (Steele, Lea, Flood 2014): small, seedable and the same on every platform.
struct Rng {
    std::uint64_t state;
    std::uint64_t next() noexcept {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double uniform() noexcept { return (static_cast<double>(next() >> 11) + 0.5) * (1.0 / 9007199254740992.0); }
    // Box-Muller: two independent standard normal values.
    std::complex<double> gaussian() noexcept {
        const double r = std::sqrt(-2.0 * std::log(uniform()));
        const double a = kTwoPi * uniform();
        return {r * std::cos(a), r * std::sin(a)};
    }
};

int wave_index(std::uint32_t i, std::uint32_t n) noexcept {
    return i < n / 2 ? static_cast<int>(i) : static_cast<int>(i) - static_cast<int>(n);
}

// e^{-+2 pi i k / n} for k < n / 2 (inverse: +).
std::vector<std::complex<float>> twiddles(std::size_t n, bool inverse) {
    std::vector<std::complex<float>> tw(n / 2);
    const double sign = inverse ? 1.0 : -1.0;
    for (std::size_t k = 0; k < n / 2; ++k) {
        const double a = sign * kTwoPi * static_cast<double>(k) / static_cast<double>(n);
        tw[k] = {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
    }
    return tw;
}

// Iterative radix-2 Cooley-Tukey over n (a power of two) values with precomputed twiddles.
void fft_with(std::complex<float>* data, std::size_t n, const std::complex<float>* tw) {
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const std::size_t half = len / 2, step = n / len;
        for (std::size_t i = 0; i < n; i += len) {
            for (std::size_t k = 0; k < half; ++k) {
                // Written out: std::complex multiplication checks for NaN/inf (Annex G) and is far slower.
                const std::complex<float> a = data[i + k], x = data[i + k + half], w = tw[k * step];
                const std::complex<float> b(x.real() * w.real() - x.imag() * w.imag(), x.real() * w.imag() + x.imag() * w.real());
                data[i + k] = {a.real() + b.real(), a.imag() + b.imag()};
                data[i + k + half] = {a.real() - b.real(), a.imag() - b.imag()};
            }
        }
    }
}

// Inverse 2D FFT of an n x n row-major grid: rows, then columns (through a transposed copy).
void fft2d(std::vector<std::complex<float>>& grid, std::uint32_t n, std::vector<std::complex<float>>& scratch,
           const std::vector<std::complex<float>>& tw) {
    for (std::uint32_t r = 0; r < n; ++r) fft_with(grid.data() + static_cast<std::size_t>(r) * n, n, tw.data());
    scratch.resize(grid.size());
    for (std::uint32_t r = 0; r < n; ++r)
        for (std::uint32_t c = 0; c < n; ++c) scratch[static_cast<std::size_t>(c) * n + r] = grid[static_cast<std::size_t>(r) * n + c];
    for (std::uint32_t c = 0; c < n; ++c) fft_with(scratch.data() + static_cast<std::size_t>(c) * n, n, tw.data());
    for (std::uint32_t r = 0; r < n; ++r)
        for (std::uint32_t c = 0; c < n; ++c) grid[static_cast<std::size_t>(r) * n + c] = scratch[static_cast<std::size_t>(c) * n + r];
}

math::Vec4 bilinear(const Field& f, const std::vector<math::Vec4>& data, math::Vec2 xz) noexcept {
    if (f.n == 0 || data.size() < static_cast<std::size_t>(f.n) * f.n || !(f.tile_size > 0)) return {};
    const float scale = static_cast<float>(f.n) / f.tile_size;
    const float u = xz.x * scale, v = xz.y * scale;
    if (!std::isfinite(u) || !std::isfinite(v)) return data[0];
    const float fu = std::floor(u), fv = std::floor(v);
    const float tu = u - fu, tv = v - fv;
    const auto n = static_cast<std::int64_t>(f.n);
    auto wrap = [n](float x) { return static_cast<std::size_t>(((static_cast<std::int64_t>(x) % n) + n) % n); };
    const std::size_t i0 = wrap(fu), i1 = (i0 + 1) % f.n, j0 = wrap(fv), j1 = (j0 + 1) % f.n;
    const math::Vec4 a = data[j0 * f.n + i0], b = data[j0 * f.n + i1], c = data[j1 * f.n + i0], d = data[j1 * f.n + i1];
    auto lerp4 = [](math::Vec4 p, math::Vec4 q, float t) {
        return math::Vec4{p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t, p.w + (q.w - p.w) * t};
    };
    return lerp4(lerp4(a, b, tu), lerp4(c, d, tu), tv);
}

} // namespace

SpectrumDesc sanitize(SpectrumDesc d) noexcept {
    std::uint32_t r = std::clamp(d.resolution, 16u, 256u);
    d.resolution = std::bit_floor(r);
    d.tile_size = std::clamp(finite_or(d.tile_size, 100.0f), 1.0f, 10000.0f);
    d.wave_height = std::clamp(finite_or(d.wave_height, 1.0f), 0.0f, 30.0f);
    d.peak_wavelength = std::clamp(finite_or(d.peak_wavelength, 20.0f), 0.1f, 2000.0f);
    d.direction = std::fmod(finite_or(d.direction, 0.0f), static_cast<float>(kTwoPi));
    d.choppiness = std::clamp(finite_or(d.choppiness, 1.0f), 0.0f, 2.0f);
    d.speed = std::clamp(finite_or(d.speed, 1.0f), 0.0f, 10.0f);
    return d;
}

bool operator==(const SpectrumDesc& a, const SpectrumDesc& b) noexcept {
    return a.resolution == b.resolution && a.tile_size == b.tile_size && a.wave_height == b.wave_height &&
           a.peak_wavelength == b.peak_wavelength && a.direction == b.direction && a.choppiness == b.choppiness &&
           a.speed == b.speed && a.seed == b.seed;
}

void fft(std::vector<std::complex<float>>& data, bool inverse) {
    const std::size_t n = data.size();
    if (n < 2 || !std::has_single_bit(n)) return;
    const std::vector<std::complex<float>> tw = twiddles(n, inverse);
    fft_with(data.data(), n, tw.data());
}

Spectrum::Spectrum(const SpectrumDesc& desc) : desc_(sanitize(desc)) {
    const std::uint32_t n = desc_.resolution;
    const std::size_t count = static_cast<std::size_t>(n) * n;
    h0_.assign(count, {});
    h0_conj_.assign(count, {});
    omega_.assign(count, 0.0);
    k_.assign(count, {});
    twiddles_ = twiddles(n, true);
    // Phillips peaks at k = 1 / (sqrt(2) L), i.e. at wavelength 2 pi sqrt(2) L.
    const double big_l = static_cast<double>(desc_.peak_wavelength) / (kTwoPi * std::numbers::sqrt2);
    // Waves within two texels of the grid's Nyquist limit are suppressed (they would alias).
    const double small_l = 2.0 * static_cast<double>(desc_.tile_size) / static_cast<double>(n) / kTwoPi;
    const double wx = std::cos(static_cast<double>(desc_.direction)), wz = std::sin(static_cast<double>(desc_.direction));
    Rng rng{0x6F6365616E000000ull ^ desc_.seed};
    for (std::uint32_t j = 0; j < n; ++j) {
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::size_t idx = static_cast<std::size_t>(j) * n + i;
            const double kx = kTwoPi * wave_index(i, n) / desc_.tile_size;
            const double kz = kTwoPi * wave_index(j, n) / desc_.tile_size;
            const double k = std::sqrt(kx * kx + kz * kz);
            const std::complex<double> xi = rng.gaussian(); // drawn for every texel so seeds stay stable
            k_[idx] = {static_cast<float>(kx), static_cast<float>(kz)};
            if (k < 1e-9) continue;
            omega_[idx] = std::sqrt(static_cast<double>(kGravity) * k) * static_cast<double>(desc_.speed);
            const double align = (kx * wx + kz * wz) / k;
            double p = std::exp(-1.0 / (k * big_l * k * big_l)) / (k * k * k * k) * align * align *
                       std::exp(-k * k * small_l * small_l);
            if (align < 0) p *= 0.07; // waves running against the wind are much weaker
            h0_[idx] = std::complex<float>(xi * std::sqrt(p * 0.5));
        }
    }
    // Scale to the requested significant wave height: 4 x the RMS height of the tile at t = 0.
    std::vector<std::complex<float>> grid(count), scratch;
    for (std::uint32_t j = 0; j < n; ++j)
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::size_t idx = static_cast<std::size_t>(j) * n + i;
            const std::size_t neg = static_cast<std::size_t>((n - j) % n) * n + (n - i) % n;
            grid[idx] = h0_[idx] + std::conj(h0_[neg]);
        }
    fft2d(grid, n, scratch, twiddles(n, true));
    double sum = 0;
    for (const auto& c : grid) sum += static_cast<double>(c.real()) * c.real();
    const double rms = std::sqrt(sum / static_cast<double>(count));
    const float scale = rms > 1e-12 ? static_cast<float>(desc_.wave_height / (4.0 * rms)) : 0.0f;
    for (auto& c : h0_) c *= scale;
    for (std::uint32_t j = 0; j < n; ++j)
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::size_t neg = static_cast<std::size_t>((n - j) % n) * n + (n - i) % n;
            h0_conj_[static_cast<std::size_t>(j) * n + i] = std::conj(h0_[neg]);
        }
}

void Spectrum::evaluate(double time, Field& out) const {
    const std::uint32_t n = desc_.resolution;
    const std::size_t count = static_cast<std::size_t>(n) * n;
    if (!std::isfinite(time)) time = 0;
    std::vector<std::complex<float>> a(count), b(count), c(count), d(count), scratch;
    for (std::size_t idx = 0; idx < count; ++idx) {
        const float kx = k_[idx].x, kz = k_[idx].y;
        const float kl = std::sqrt(kx * kx + kz * kz);
        if (kl < 1e-9f) continue;
        const auto phase = static_cast<float>(std::fmod(omega_[idx] * time, kTwoPi));
        const float cs = std::cos(phase), sn = std::sin(phase);
        // h~ = h0 e^{i phase} + conj(h0(-k)) e^{-i phase}, written out (see fft_with).
        const std::complex<float> p = h0_[idx], q = h0_conj_[idx];
        const float hr = p.real() * cs - p.imag() * sn + q.real() * cs + q.imag() * sn;
        const float hi = p.real() * sn + p.imag() * cs - q.real() * sn + q.imag() * cs;
        const float ux = kx / kl, uz = kz / kl;
        // Pairs (f1 + i f2): (height, Dx), (Dz, slope x), (slope z, dDx/dx), (dDz/dz, dDx/dz),
        // with D = sum -i k^ h~ (Tessendorf eq. 29) and derivatives i k:
        //   a = h~ (1 + kx/k), b = -i (kz/k) h~ - kx h~, c = i h~ (kz + kx^2/k), d = h~ (kz^2 + i kx kz) / k.
        a[idx] = {hr * (1.0f + ux), hi * (1.0f + ux)};
        b[idx] = {uz * hi - kx * hr, -uz * hr - kx * hi};
        const float cz = kz + kx * ux;
        c[idx] = {-hi * cz, hr * cz};
        const float dr = kz * uz, di = kx * uz;
        d[idx] = {hr * dr - hi * di, hr * di + hi * dr};
    }
    fft2d(a, n, scratch, twiddles_);
    fft2d(b, n, scratch, twiddles_);
    fft2d(c, n, scratch, twiddles_);
    fft2d(d, n, scratch, twiddles_);
    out.n = n;
    out.tile_size = desc_.tile_size;
    out.time = time;
    out.revision = g_revision.fetch_add(1, std::memory_order_relaxed) + 1;
    out.displacement.resize(count);
    out.normal.resize(count);
    out.max_height = 0;
    out.max_horizontal = 0;
    // x = x0 - lambda D moves points towards the crests (sharpening them, like Gerstner Q > 0).
    const float lam = -desc_.choppiness;
    for (std::size_t idx = 0; idx < count; ++idx) {
        const float h = a[idx].real(), dx = lam * a[idx].imag(), dz = lam * b[idx].real();
        const float sx = b[idx].imag(), sz = c[idx].real();
        const float jxx = lam * c[idx].imag(), jzz = lam * d[idx].real(), jxz = lam * d[idx].imag();
        out.displacement[idx] = {dx, h, dz, 0};
        // Tangents of the displaced surface along the rest X and Z axes; normal = Tz x Tx.
        const math::Vec3 tx{1.0f + jxx, sx, jxz}, tz{jxz, sz, 1.0f + jzz};
        math::Vec3 nrm = math::cross(tz, tx);
        const float len = math::length(nrm);
        nrm = len > 1e-6f && nrm.y > 0 ? nrm * (1.0f / len) : math::Vec3{0, 1, 0};
        out.normal[idx] = {nrm.x, nrm.y, nrm.z, (1.0f + jxx) * (1.0f + jzz) - jxz * jxz};
        out.max_height = std::max(out.max_height, std::abs(h));
        out.max_horizontal = std::max(out.max_horizontal, std::sqrt(dx * dx + dz * dz));
    }
}

math::Vec4 displacement_at(const Field& f, math::Vec2 xz) noexcept { return bilinear(f, f.displacement, xz); }
math::Vec4 normal_at(const Field& f, math::Vec2 xz) noexcept { return bilinear(f, f.normal, xz); }

Sample sample(const Field& f, math::Vec2 xz) noexcept {
    Sample s;
    if (f.n == 0) return s;
    math::Vec2 p0 = xz;
    for (int it = 0; it < 6; ++it) {
        const math::Vec4 d = displacement_at(f, p0);
        p0 = {xz.x - d.x, xz.y - d.z};
    }
    s.height = displacement_at(f, p0).y;
    const math::Vec4 nw = normal_at(f, p0);
    const math::Vec3 nrm{nw.x, nw.y, nw.z};
    const float len = math::length(nrm);
    s.normal = len > 1e-6f ? nrm * (1.0f / len) : math::Vec3{0, 1, 0};
    s.jacobian = nw.w;
    return s;
}

} // namespace victus::ocean
