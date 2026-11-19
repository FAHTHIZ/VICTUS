// SPDX-License-Identifier: Apache-2.0
// Perceptual image and sequence comparison (ADR-0102): how different a rendered frame looks from
// a reference, for the perceptual-equivalence goal (docs/handoff/VICTUS_PERCEPTUAL_EQUIVALENCE_RENDERING.md).
//
// FLIP (NVIDIA, HPG 2020) approximates the difference a viewer sees when flipping between two
// images at a given viewing distance; SSIM (Wang et al. 2004) measures structural change; PSNR
// is reported for diagnosis only. Means hide local breakage, so every metric is also pooled per
// tile and the worst tile is reported. Sequences add a temporal term: how much the test's
// frame-to-frame change differs from the reference's (flicker, ghosting, popping).
#pragma once

#include "victus/core/result.hpp"
#include "victus/render/image.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace victus::render {

// Viewing condition for FLIP: pixels per degree of visual angle. The FLIP default (0.7 m from a
// 0.7 m wide 3840-pixel monitor) is about 67.
[[nodiscard]] float pixels_per_degree(float distance_m, float resolution_x, float monitor_width_m) noexcept;

struct CompareOptions {
    float pixels_per_degree{0}; // 0 = FLIP's default viewing condition
    std::uint32_t tile{64};     // pooling tile size in pixels (clamped to the image)
    float visible_flip{0.1f};   // a pixel whose FLIP error exceeds this counts as a visible difference
    bool keep_maps{false};      // keep the per-pixel FLIP map (for heatmaps)
};

struct ImageComparison {
    std::uint32_t width{0}, height{0};
    double flip_mean{0};       // mean FLIP error, 0 = identical, 1 = maximal
    double flip_p95{0};        // 95th percentile of per-pixel FLIP
    double flip_max{0};
    double flip_worst_tile{0}; // highest tile mean FLIP
    double ssim{1};            // mean SSIM of luma (1 = identical)
    double ssim_worst_tile{1}; // lowest tile mean SSIM
    double psnr{0};            // RGB PSNR in dB (infinity when identical)
    double visible_fraction{0}; // pixels with FLIP above CompareOptions::visible_flip
    std::uint32_t worst_tile_x{0}, worst_tile_y{0}, worst_tile_w{0}, worst_tile_h{0}; // by FLIP
    std::vector<float> flip_map; // width*height, when CompareOptions::keep_maps
};

// Images must have the same size, at least 11x11 (SSIM window). Alpha is ignored.
[[nodiscard]] Result<ImageComparison> compare_images(const Image& reference, const Image& test,
                                                     const CompareOptions& options = {});

// FLIP map as an RGBA image in FLIP's magma colour map (black = no visible difference).
[[nodiscard]] Image flip_heatmap(const ImageComparison& comparison);

struct SequenceComparison {
    std::vector<ImageComparison> frames;
    double flip_mean{0};       // mean over frames
    double flip_worst_frame{0};
    std::size_t worst_frame{0};
    double flip_worst_tile{0}; // worst tile over all frames
    double ssim_worst_tile{1};
    // Mean |Δtest - Δref| of linear luminance between consecutive frames (0..1), worst pair.
    // Zero when the test changes over time exactly as the reference does.
    double temporal_error{0};
    std::size_t temporal_worst_frame{0}; // index of the later frame of the worst pair
};

[[nodiscard]] Result<SequenceComparison> compare_sequences(std::span<const Image> reference, std::span<const Image> test,
                                                           const CompareOptions& options = {});

// Tolerances for `victus compare` and tests; unset (negative) limits are not checked.
struct CompareLimits {
    double max_flip_mean{-1}, max_flip_tile{-1}, min_ssim{-1}, min_ssim_tile{-1}, max_temporal{-1};
};
// Human-readable reasons the comparison is outside the limits; empty = within.
[[nodiscard]] std::vector<std::string> check_limits(const SequenceComparison& result, const CompareLimits& limits);

} // namespace victus::render
