// SPDX-License-Identifier: Apache-2.0
#include "victus/render/image_compare.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>

namespace victus_flip {
std::vector<float> ldr_error_map(const float* reference, const float* test, int width, int height, float ppd);
float pixels_per_degree(float distance_m, float resolution_x, float width_m);
void magma(float value, float rgb[3]);
} // namespace victus_flip

namespace victus::render {

namespace {

constexpr int kSsimRadius = 5; // 11-tap Gaussian, sigma 1.5 (Wang et al. 2004)

const std::array<float, 256>& srgb_to_linear_table() {
    static const std::array<float, 256> table = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const float c = static_cast<float>(i) / 255.0f;
            t[static_cast<std::size_t>(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    return table;
}

std::vector<float> linear_rgb(const Image& img) {
    const auto& lut = srgb_to_linear_table();
    const std::size_t n = static_cast<std::size_t>(img.width) * img.height;
    std::vector<float> out(n * 3);
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t c = 0; c < 3; ++c) out[i * 3 + c] = lut[img.pixels[i * 4 + c]];
    return out;
}

std::vector<float> encoded_luma(const Image& img) {
    const std::size_t n = static_cast<std::size_t>(img.width) * img.height;
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i)
        out[i] = 0.2126f * img.pixels[i * 4] + 0.7152f * img.pixels[i * 4 + 1] + 0.0722f * img.pixels[i * 4 + 2];
    return out;
}

std::vector<float> linear_luma(const Image& img) {
    const auto& lut = srgb_to_linear_table();
    const std::size_t n = static_cast<std::size_t>(img.width) * img.height;
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i)
        out[i] = 0.2126f * lut[img.pixels[i * 4]] + 0.7152f * lut[img.pixels[i * 4 + 1]] + 0.0722f * lut[img.pixels[i * 4 + 2]];
    return out;
}

// Separable Gaussian blur with clamped edges (only the interior is used for SSIM).
std::vector<float> gaussian(const std::vector<float>& src, int w, int h) {
    constexpr std::size_t taps = 2 * kSsimRadius + 1;
    std::array<float, taps> k{};
    float sum = 0;
    for (std::size_t j = 0; j < taps; ++j) {
        const auto d = static_cast<float>(static_cast<int>(j) - kSsimRadius);
        k[j] = std::exp(-d * d / (2.0f * 1.5f * 1.5f));
        sum += k[j];
    }
    for (float& v : k) v /= sum;
    const auto at = [w](int x, int y) { return static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x); };
    std::vector<float> tmp(src.size()), out(src.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float acc = 0;
            for (std::size_t j = 0; j < taps; ++j) acc += k[j] * src[at(std::clamp(x + static_cast<int>(j) - kSsimRadius, 0, w - 1), y)];
            tmp[at(x, y)] = acc;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float acc = 0;
            for (std::size_t j = 0; j < taps; ++j) acc += k[j] * tmp[at(x, std::clamp(y + static_cast<int>(j) - kSsimRadius, 0, h - 1))];
            out[at(x, y)] = acc;
        }
    return out;
}

std::vector<float> ssim_map(const Image& a, const Image& b) {
    const int w = static_cast<int>(a.width), h = static_cast<int>(a.height);
    const auto x = encoded_luma(a), y = encoded_luma(b);
    std::vector<float> xx(x.size()), yy(x.size()), xy(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        xx[i] = x[i] * x[i];
        yy[i] = y[i] * y[i];
        xy[i] = x[i] * y[i];
    }
    const auto mx = gaussian(x, w, h), my = gaussian(y, w, h), sxx = gaussian(xx, w, h), syy = gaussian(yy, w, h),
               sxy = gaussian(xy, w, h);
    constexpr float c1 = (0.01f * 255.0f) * (0.01f * 255.0f), c2 = (0.03f * 255.0f) * (0.03f * 255.0f);
    std::vector<float> s(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float vx = std::max(sxx[i] - mx[i] * mx[i], 0.0f), vy = std::max(syy[i] - my[i] * my[i], 0.0f);
        const float cov = sxy[i] - mx[i] * my[i];
        s[i] = ((2 * mx[i] * my[i] + c1) * (2 * cov + c2)) / ((mx[i] * mx[i] + my[i] * my[i] + c1) * (vx + vy + c2));
    }
    return s;
}

// Tile edges: about `tile` pixels each, the remainder spread evenly, so no tile is a thin strip.
std::vector<std::uint32_t> tile_edges(std::uint32_t size, std::uint32_t tile) {
    const std::uint32_t t = std::clamp<std::uint32_t>(tile, 8, std::max<std::uint32_t>(size, 8));
    const std::uint32_t n = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(double(size) / t)));
    std::vector<std::uint32_t> e(n + 1);
    for (std::uint32_t i = 0; i <= n; ++i) e[i] = static_cast<std::uint32_t>(std::uint64_t(size) * i / n);
    return e;
}

} // namespace

float pixels_per_degree(float distance_m, float resolution_x, float monitor_width_m) noexcept {
    return victus_flip::pixels_per_degree(distance_m, resolution_x, monitor_width_m);
}

Result<ImageComparison> compare_images(const Image& reference, const Image& test, const CompareOptions& options) {
    if (reference.width != test.width || reference.height != test.height)
        return make_error(ErrorCode::InvalidArgument, std::format("image sizes differ: reference {}x{}, test {}x{}", reference.width,
                                                                  reference.height, test.width, test.height));
    const std::size_t n = static_cast<std::size_t>(reference.width) * reference.height;
    if (reference.width < 11 || reference.height < 11 || reference.pixels.size() != n * 4 || test.pixels.size() != n * 4)
        return make_error(ErrorCode::InvalidArgument, "images must be RGBA and at least 11x11 pixels");
    ImageComparison r;
    r.width = reference.width;
    r.height = reference.height;
    const int w = static_cast<int>(r.width), h = static_cast<int>(r.height);

    const float ppd = options.pixels_per_degree > 0 ? options.pixels_per_degree : victus_flip::pixels_per_degree(0.7f, 3840.0f, 0.7f);
    const auto ref_lin = linear_rgb(reference), test_lin = linear_rgb(test);
    std::vector<float> flip = victus_flip::ldr_error_map(ref_lin.data(), test_lin.data(), w, h, ppd);
    // Identical inputs must score exactly zero. Optimised builds may evaluate the reference and
    // test filter chains with different floating-point contraction, leaving ~1e-7 residue that
    // would fail a zero-tolerance determinism check, so pixel-identical images and sub-epsilon
    // residue are snapped to zero.
    if (reference.pixels == test.pixels) std::fill(flip.begin(), flip.end(), 0.0f);
    for (float& e : flip)
        if (e < 1e-6f) e = 0.0f;
    const auto ssim = ssim_map(reference, test);

    double flip_sum = 0, sq = 0;
    std::size_t visible = 0;
    for (std::size_t i = 0; i < n; ++i) {
        flip_sum += flip[i];
        r.flip_max = std::max<double>(r.flip_max, flip[i]);
        if (flip[i] > options.visible_flip) ++visible;
        for (std::size_t c = 0; c < 3; ++c) {
            const double d = double(reference.pixels[i * 4 + c]) - double(test.pixels[i * 4 + c]);
            sq += d * d;
        }
    }
    r.flip_mean = flip_sum / double(n);
    r.visible_fraction = double(visible) / double(n);
    r.psnr = sq == 0 ? std::numeric_limits<double>::infinity() : 10.0 * std::log10(255.0 * 255.0 / (sq / double(n * 3)));
    {
        std::vector<float> sorted = flip;
        const auto k = static_cast<std::ptrdiff_t>(std::min(n - 1, static_cast<std::size_t>(std::ceil(0.95 * double(n))) - 1));
        std::nth_element(sorted.begin(), sorted.begin() + k, sorted.end());
        r.flip_p95 = sorted[static_cast<std::size_t>(k)];
    }
    // SSIM: interior pixels only (the window must fit), as in Wang et al. and scikit-image.
    double ssim_sum = 0;
    std::size_t ssim_n = 0;
    for (int y = kSsimRadius; y < h - kSsimRadius; ++y)
        for (int x = kSsimRadius; x < w - kSsimRadius; ++x) {
            ssim_sum += ssim[static_cast<std::size_t>(y) * w + x];
            ++ssim_n;
        }
    r.ssim = ssim_n ? ssim_sum / double(ssim_n) : 1.0;

    const auto ex = tile_edges(r.width, options.tile), ey = tile_edges(r.height, options.tile);
    r.flip_worst_tile = -1;
    r.ssim_worst_tile = 2;
    for (std::size_t ty = 0; ty + 1 < ey.size(); ++ty)
        for (std::size_t tx = 0; tx + 1 < ex.size(); ++tx) {
            double fs = 0, ss = 0;
            std::size_t fn = 0, sn = 0;
            for (std::uint32_t y = ey[ty]; y < ey[ty + 1]; ++y)
                for (std::uint32_t x = ex[tx]; x < ex[tx + 1]; ++x) {
                    const std::size_t i = std::size_t(y) * r.width + x;
                    fs += flip[i];
                    ++fn;
                    if (int(x) >= kSsimRadius && int(x) < w - kSsimRadius && int(y) >= kSsimRadius && int(y) < h - kSsimRadius) {
                        ss += ssim[i];
                        ++sn;
                    }
                }
            const double fm = fn ? fs / double(fn) : 0;
            if (fm > r.flip_worst_tile) {
                r.flip_worst_tile = fm;
                r.worst_tile_x = ex[tx];
                r.worst_tile_y = ey[ty];
                r.worst_tile_w = ex[tx + 1] - ex[tx];
                r.worst_tile_h = ey[ty + 1] - ey[ty];
            }
            if (sn) r.ssim_worst_tile = std::min(r.ssim_worst_tile, ss / double(sn));
        }
    if (r.ssim_worst_tile > 1.5) r.ssim_worst_tile = r.ssim;
    if (options.keep_maps) r.flip_map = std::move(flip);
    return r;
}

Image flip_heatmap(const ImageComparison& c) {
    Image out{c.width, c.height, {}};
    if (c.flip_map.size() != std::size_t(c.width) * c.height) return out;
    out.pixels.resize(c.flip_map.size() * 4);
    for (std::size_t i = 0; i < c.flip_map.size(); ++i) {
        float rgb[3];
        victus_flip::magma(c.flip_map[i], rgb);
        for (std::size_t k = 0; k < 3; ++k)
            out.pixels[i * 4 + k] = static_cast<std::uint8_t>(std::lround(std::clamp(rgb[k], 0.0f, 1.0f) * 255.0f));
        out.pixels[i * 4 + 3] = 255;
    }
    return out;
}

Result<SequenceComparison> compare_sequences(std::span<const Image> reference, std::span<const Image> test,
                                             const CompareOptions& options) {
    if (reference.empty() || reference.size() != test.size())
        return make_error(ErrorCode::InvalidArgument,
                          std::format("sequences need the same, nonzero number of frames (reference {}, test {})", reference.size(), test.size()));
    SequenceComparison s;
    s.ssim_worst_tile = 1;
    std::vector<float> prev_ref, prev_test;
    for (std::size_t f = 0; f < reference.size(); ++f) {
        auto c = compare_images(reference[f], test[f], options);
        if (!c) return make_error(c.error().code, std::format("frame {}: {}", f, c.error().message));
        s.flip_mean += c->flip_mean;
        if (c->flip_mean > s.flip_worst_frame || f == 0) {
            s.flip_worst_frame = c->flip_mean;
            s.worst_frame = f;
        }
        s.flip_worst_tile = std::max(s.flip_worst_tile, c->flip_worst_tile);
        s.ssim_worst_tile = std::min(s.ssim_worst_tile, c->ssim_worst_tile);
        auto ref_y = linear_luma(reference[f]), test_y = linear_luma(test[f]);
        if (f > 0) {
            if (prev_ref.size() != ref_y.size())
                return make_error(ErrorCode::InvalidArgument, std::format("frame {} has a different size from frame {}", f, f - 1));
            double sum = 0;
            for (std::size_t i = 0; i < ref_y.size(); ++i)
                sum += std::abs((test_y[i] - prev_test[i]) - (ref_y[i] - prev_ref[i]));
            const double t = sum / double(ref_y.size());
            if (t > s.temporal_error) {
                s.temporal_error = t;
                s.temporal_worst_frame = f;
            }
        }
        prev_ref = std::move(ref_y);
        prev_test = std::move(test_y);
        s.frames.push_back(std::move(*c));
    }
    s.flip_mean /= double(s.frames.size());
    return s;
}

std::vector<std::string> check_limits(const SequenceComparison& r, const CompareLimits& l) {
    std::vector<std::string> out;
    if (l.max_flip_mean >= 0 && r.flip_worst_frame > l.max_flip_mean)
        out.push_back(std::format("mean FLIP {:.4f} (frame {}) is above {:.4f}", r.flip_worst_frame, r.worst_frame, l.max_flip_mean));
    if (l.max_flip_tile >= 0 && r.flip_worst_tile > l.max_flip_tile)
        out.push_back(std::format("worst-tile FLIP {:.4f} is above {:.4f}", r.flip_worst_tile, l.max_flip_tile));
    double ssim_min = 1;
    for (const auto& f : r.frames) ssim_min = std::min(ssim_min, f.ssim);
    if (l.min_ssim >= 0 && ssim_min < l.min_ssim) out.push_back(std::format("SSIM {:.4f} is below {:.4f}", ssim_min, l.min_ssim));
    if (l.min_ssim_tile >= 0 && r.ssim_worst_tile < l.min_ssim_tile)
        out.push_back(std::format("worst-tile SSIM {:.4f} is below {:.4f}", r.ssim_worst_tile, l.min_ssim_tile));
    if (l.max_temporal >= 0 && r.temporal_error > l.max_temporal)
        out.push_back(std::format("temporal error {:.4f} (frame {}) is above {:.4f}", r.temporal_error, r.temporal_worst_frame, l.max_temporal));
    return out;
}

} // namespace victus::render
