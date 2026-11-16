// SPDX-License-Identifier: Apache-2.0
// CPU images: decode (PNG/JPEG/TGA/BMP/HDR via stb_image), PNG encode, mip generation.
#pragma once
#include "victus/core/fs.hpp"
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
[[nodiscard]] std::uint32_t mip_count(std::uint32_t width, std::uint32_t height) noexcept;
// Procedural checkerboard used by templates/tests.
[[nodiscard]] Image make_checker(std::uint32_t size, std::uint32_t cells, std::uint32_t a, std::uint32_t b);

} // namespace victus::render
