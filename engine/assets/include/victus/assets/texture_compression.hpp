// SPDX-License-Identifier: Apache-2.0
// GPU block compression for cooked textures (ADR-0047).
//
// The cooker encodes each texture's CPU mip chain into one or more block-compressed variants:
// BC1/BC3/BC7 for desktop GPUs (bc7enc_rdo) and ASTC 4x4/6x6 for mobile GPUs (Arm ASTC
// Encoder). SDL_GPU exposes no ETC2 formats, so ASTC is the mobile path. At load time the
// runtime picks the first variant the device can sample and falls back to the RGBA8 mips.
#pragma once
#include "victus/core/result.hpp"
#include "victus/render/image.hpp"
#include "victus/rhi/rhi.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace victus::assets {

enum class TextureCodec : std::uint8_t { BC1 = 1, BC3 = 2, BC7 = 3, ASTC4x4 = 4, ASTC6x6 = 5 };
[[nodiscard]] const char* to_string(TextureCodec codec) noexcept;
[[nodiscard]] std::optional<TextureCodec> parse_texture_codec(std::string_view name) noexcept;
[[nodiscard]] rhi::Format codec_format(TextureCodec codec, bool srgb) noexcept;
[[nodiscard]] bool is_desktop_codec(TextureCodec codec) noexcept; // BC family

// Parses an import setting: "none", "auto" (BC7 + ASTC 4x4), "small" (BC1 + ASTC 6x6) or a
// '+'-separated codec list such as "bc7+astc6x6". Unknown names are an error.
[[nodiscard]] Result<std::vector<TextureCodec>> parse_compression_setting(std::string_view setting);

enum class CompressionEffort : std::uint8_t { Fast, Thorough };

// Encodes one RGBA8 image (any size; partial edge blocks replicate the border texels). `srgb`
// selects perceptual/sRGB encoding. Returns rhi::mip_size_bytes(codec_format(...)) bytes.
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_texture(const render::Image& image, TextureCodec codec, bool srgb,
                                                              CompressionEffort effort = CompressionEffort::Fast);
// Decodes blocks back to RGBA8 (tests, previews and software fallbacks).
[[nodiscard]] Result<render::Image> decode_texture(std::span<const std::uint8_t> blocks, std::uint32_t width,
                                                  std::uint32_t height, TextureCodec codec, bool srgb);

// Peak signal-to-noise ratio (dB) over RGB, or RGBA when `alpha`. Infinity for identical images.
[[nodiscard]] double image_psnr(const render::Image& a, const render::Image& b, bool alpha = false);

struct CompressedMips {
    TextureCodec codec{TextureCodec::BC7};
    std::vector<std::vector<std::uint8_t>> levels; // one per CookedTexture::mips entry
};

// Encodes a whole mip chain. BC1 is promoted to BC3 when any texel is not opaque.
[[nodiscard]] Result<CompressedMips> compress_mips(std::span<const render::Image> mips, TextureCodec codec, bool srgb,
                                                   CompressionEffort effort = CompressionEffort::Fast);

} // namespace victus::assets
