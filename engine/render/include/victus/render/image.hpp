// SPDX-License-Identifier: Apache-2.0
// CPU images: decode (PNG/JPEG/TGA/BMP/HDR via stb_image), PNG/HDR encode, mip generation,
// float (HDR) images and half-float conversion.
#pragma once
#include "victus/core/fs.hpp"
#include "victus/core/math.hpp"
#include "victus/core/result.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace victus::render {

struct Image {
    std::uint32_t width{0}, height{0};
    std::vector<std::uint8_t> pixels; // RGBA8, row-major, top row first
    [[nodiscard]] bool empty() const noexcept { return pixels.empty(); }
    [[nodiscard]] std::uint32_t pixel(std::uint32_t x, std::uint32_t y) const noexcept; // packed 0xAABBGGRR
};

[[nodiscard]] Result<Image> decode_image(std::span<const std::uint8_t> encoded);
[[nodiscard]] Result<Image> load_image(const fs::Path& path);
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_png(const Image& image);
[[nodiscard]] Status save_png(const fs::Path& path, const Image& image);
// Box-filtered mip chain (mip 0 = source). `srgb` filters in linear space.
[[nodiscard]] std::vector<Image> build_mip_chain(const Image& base, bool srgb);
// Fraction of texels whose alpha passes an alpha test at `cutoff` (alpha >= cutoff, 0..1).
[[nodiscard]] float alpha_coverage(const Image& image, float cutoff) noexcept;
// Coverage-preserving alpha mips (Castano 2010): rescales the alpha of chain[1..] so the fraction of
// texels passing an alpha test at `cutoff` matches chain[0]. Box-filtered alpha otherwise thins out
// with each level, so alpha-tested foliage and fences fade away as the camera zooms out. No-op
// when cutoff is outside (0, 1) or the chain has one level.
void preserve_alpha_coverage(std::span<Image> chain, float cutoff) noexcept;
[[nodiscard]] std::uint32_t mip_count(std::uint32_t width, std::uint32_t height) noexcept;
// High dynamic range images (linear RGBA float), e.g. Radiance .hdr environment maps.
struct ImageF {
    std::uint32_t width{0}, height{0};
    std::vector<float> pixels; // RGBA32F linear, row-major, top row first
    [[nodiscard]] bool empty() const noexcept { return pixels.empty(); }
    [[nodiscard]] math::Vec4 at(std::uint32_t x, std::uint32_t y) const noexcept;
};
// True when the encoded bytes are a floating-point format stb_image decodes as HDR (.hdr).
[[nodiscard]] bool is_hdr_image(std::span<const std::uint8_t> encoded) noexcept;
[[nodiscard]] Result<ImageF> decode_image_hdr(std::span<const std::uint8_t> encoded);
[[nodiscard]] Result<ImageF> load_image_hdr(const fs::Path& path);
// Radiance RGBE encoding (alpha dropped).
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_hdr(const ImageF& image);
// Box-filters by integer factors until both sides fit `max_width` x `max_height`.
[[nodiscard]] ImageF downsample_to_fit(const ImageF& image, std::uint32_t max_width, std::uint32_t max_height);
// IEEE 754 binary16 conversion (round to nearest even, clamps to the finite range).
[[nodiscard]] std::uint16_t float_to_half(float f) noexcept;
[[nodiscard]] float half_to_float(std::uint16_t h) noexcept;
// RGBA16F bytes of an HDR image (for RGBA16_FLOAT textures).
[[nodiscard]] std::vector<std::uint8_t> to_rgba16f(const ImageF& image);

// Procedural checkerboard used by templates/tests.
[[nodiscard]] Image make_checker(std::uint32_t size, std::uint32_t cells, std::uint32_t a, std::uint32_t b);

} // namespace victus::render
