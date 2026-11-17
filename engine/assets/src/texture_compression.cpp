// SPDX-License-Identifier: Apache-2.0
#include "victus/assets/texture_compression.hpp"

#include "astcenc.h"
#include "bc7decomp.h"
#include "bc7enc.h"
#include "rgbcx.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <mutex>
#include <thread>

namespace victus::assets {

const char* to_string(TextureCodec codec) noexcept {
    switch (codec) {
    case TextureCodec::BC1: return "bc1";
    case TextureCodec::BC3: return "bc3";
    case TextureCodec::BC7: return "bc7";
    case TextureCodec::ASTC4x4: return "astc4x4";
    case TextureCodec::ASTC6x6: return "astc6x6";
    }
    return "unknown";
}

std::optional<TextureCodec> parse_texture_codec(std::string_view name) noexcept {
    for (const auto c : {TextureCodec::BC1, TextureCodec::BC3, TextureCodec::BC7, TextureCodec::ASTC4x4, TextureCodec::ASTC6x6})
        if (name == to_string(c)) return c;
    return std::nullopt;
}

rhi::Format codec_format(TextureCodec codec, bool srgb) noexcept {
    using F = rhi::Format;
    switch (codec) {
    case TextureCodec::BC1: return srgb ? F::BC1_RGBA_SRGB : F::BC1_RGBA_UNORM;
    case TextureCodec::BC3: return srgb ? F::BC3_RGBA_SRGB : F::BC3_RGBA_UNORM;
    case TextureCodec::BC7: return srgb ? F::BC7_RGBA_SRGB : F::BC7_RGBA_UNORM;
    case TextureCodec::ASTC4x4: return srgb ? F::ASTC_4x4_SRGB : F::ASTC_4x4_UNORM;
    case TextureCodec::ASTC6x6: return srgb ? F::ASTC_6x6_SRGB : F::ASTC_6x6_UNORM;
    }
    return F::RGBA8_UNORM;
}

bool is_desktop_codec(TextureCodec codec) noexcept {
    return codec == TextureCodec::BC1 || codec == TextureCodec::BC3 || codec == TextureCodec::BC7;
}

Result<std::vector<TextureCodec>> parse_compression_setting(std::string_view setting) {
    std::vector<TextureCodec> out;
    if (setting.empty() || setting == "none") return out;
    if (setting == "auto") return std::vector{TextureCodec::BC7, TextureCodec::ASTC4x4};
    if (setting == "small") return std::vector{TextureCodec::BC1, TextureCodec::ASTC6x6};
    for (;;) {
        const auto plus = setting.find('+');
        const std::string_view name = setting.substr(0, plus);
        const auto c = parse_texture_codec(name);
        if (!c) return make_error(ErrorCode::InvalidArgument, std::format("unknown texture compression '{}'", name));
        if (std::find(out.begin(), out.end(), *c) == out.end()) out.push_back(*c);
        if (plus == std::string_view::npos) break;
        setting.remove_prefix(plus + 1);
    }
    return out;
}

namespace {

std::once_flag g_bc_init;
void init_bc() {
    std::call_once(g_bc_init, [] {
        rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
        bc7enc_compress_block_init();
    });
}

std::uint32_t worker_count(std::uint32_t rows) {
    const std::uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
    return std::clamp(std::min(hw, 8u), 1u, std::max(1u, rows / 4));
}

template <typename Fn> void parallel_rows(std::uint32_t rows, Fn fn) {
    const std::uint32_t n = worker_count(rows);
    if (n <= 1) {
        fn(0u, rows);
        return;
    }
    std::vector<std::thread> threads;
    threads.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) threads.emplace_back([&, i] { fn(rows * i / n, rows * (i + 1) / n); });
    for (auto& t : threads) t.join();
}

// Copies a 4x4 block (clamped at the image edge) into `out` as 16 RGBA texels.
void fetch_block(const render::Image& img, std::uint32_t bx, std::uint32_t by, std::uint8_t out[64]) {
    for (std::uint32_t y = 0; y < 4; ++y) {
        const std::uint32_t sy = std::min(by * 4 + y, img.height - 1);
        for (std::uint32_t x = 0; x < 4; ++x) {
            const std::uint32_t sx = std::min(bx * 4 + x, img.width - 1);
            std::memcpy(out + (y * 4 + x) * 4, img.pixels.data() + (std::size_t{sy} * img.width + sx) * 4, 4);
        }
    }
}

struct AstcContext {
    astcenc_context* ctx{nullptr};
    ~AstcContext() {
        if (ctx) astcenc_context_free(ctx);
    }
};

Result<astcenc_config> astc_config(TextureCodec codec, bool srgb, float quality, unsigned flags) {
    const unsigned b = codec == TextureCodec::ASTC6x6 ? 6 : 4;
    astcenc_config cfg{};
    const auto st = astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, b, b, 1, quality, flags, &cfg);
    if (st != ASTCENC_SUCCESS) return make_error(ErrorCode::InvalidArgument, std::format("astcenc config: {}", astcenc_get_error_string(st)));
    return cfg;
}

constexpr astcenc_swizzle kIdentity{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};

Result<std::vector<std::uint8_t>> encode_astc(const render::Image& image, TextureCodec codec, bool srgb, CompressionEffort effort) {
    auto cfg = astc_config(codec, srgb, effort == CompressionEffort::Thorough ? ASTCENC_PRE_MEDIUM : ASTCENC_PRE_FAST, 0);
    if (!cfg) return std::move(cfg).error();
    const unsigned b = cfg->block_x;
    const std::uint32_t threads = worker_count((image.height + b - 1) / b);
    AstcContext c;
    const auto st = astcenc_context_alloc(&*cfg, threads, &c.ctx, nullptr);
    if (st != ASTCENC_SUCCESS) return make_error(ErrorCode::InvalidArgument, std::format("astcenc context: {}", astcenc_get_error_string(st)));
    std::vector<std::uint8_t> out(rhi::mip_size_bytes(codec_format(codec, srgb), image.width, image.height));
    void* slice = const_cast<std::uint8_t*>(image.pixels.data());
    astcenc_image img{image.width, image.height, 1, ASTCENC_TYPE_U8, &slice};
    std::vector<astcenc_error> errors(threads, ASTCENC_SUCCESS);
    std::vector<std::thread> pool;
    for (std::uint32_t i = 1; i < threads; ++i)
        pool.emplace_back([&, i] { errors[i] = astcenc_compress_image(c.ctx, &img, &kIdentity, out.data(), out.size(), i); });
    errors[0] = astcenc_compress_image(c.ctx, &img, &kIdentity, out.data(), out.size(), 0);
    for (auto& t : pool) t.join();
    for (const auto e : errors)
        if (e != ASTCENC_SUCCESS) return make_error(ErrorCode::InvalidArgument, std::format("astcenc compress: {}", astcenc_get_error_string(e)));
    return out;
}

} // namespace

Result<std::vector<std::uint8_t>> encode_texture(const render::Image& image, TextureCodec codec, bool srgb, CompressionEffort effort) {
    if (image.width == 0 || image.height == 0 || image.pixels.size() != std::size_t{image.width} * image.height * 4)
        return make_error(ErrorCode::InvalidArgument, "encode_texture: image has no RGBA8 pixels");
    if (!is_desktop_codec(codec)) return encode_astc(image, codec, srgb, effort);
    init_bc();
    const std::uint32_t bw = (image.width + 3) / 4, bh = (image.height + 3) / 4;
    const std::uint32_t block_bytes = codec == TextureCodec::BC1 ? 8 : 16;
    std::vector<std::uint8_t> out(std::size_t{bw} * bh * block_bytes);
    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    if (!srgb) bc7enc_compress_block_params_init_linear_weights(&params);
    params.m_uber_level = effort == CompressionEffort::Thorough ? 2 : 0;
    params.m_max_partitions = effort == CompressionEffort::Thorough ? 64 : 16;
    const std::uint32_t bc_level = effort == CompressionEffort::Thorough ? 18 : 10;
    parallel_rows(bh, [&](std::uint32_t y0, std::uint32_t y1) {
        std::uint8_t px[64];
        for (std::uint32_t by = y0; by < y1; ++by)
            for (std::uint32_t bx = 0; bx < bw; ++bx) {
                fetch_block(image, bx, by, px);
                std::uint8_t* dst = out.data() + (std::size_t{by} * bw + bx) * block_bytes;
                switch (codec) {
                case TextureCodec::BC1: rgbcx::encode_bc1(bc_level, dst, px, false, false); break;
                case TextureCodec::BC3: rgbcx::encode_bc3(bc_level, dst, px); break;
                default: bc7enc_compress_block(dst, px, &params); break;
                }
            }
    });
    return out;
}

Result<render::Image> decode_texture(std::span<const std::uint8_t> blocks, std::uint32_t width, std::uint32_t height, TextureCodec codec,
                                     bool srgb) {
    if (width == 0 || height == 0) return make_error(ErrorCode::InvalidArgument, "decode_texture: empty size");
    if (blocks.size() != rhi::mip_size_bytes(codec_format(codec, srgb), width, height))
        return make_error(ErrorCode::InvalidArgument, std::format("decode_texture: {} bytes do not match {}x{} {}", blocks.size(), width,
                                                                  height, to_string(codec)));
    render::Image img;
    img.width = width;
    img.height = height;
    img.pixels.resize(std::size_t{width} * height * 4);
    if (!is_desktop_codec(codec)) {
        auto cfg = astc_config(codec, srgb, ASTCENC_PRE_FASTEST, ASTCENC_FLG_DECOMPRESS_ONLY);
        if (!cfg) return std::move(cfg).error();
        AstcContext c;
        auto st = astcenc_context_alloc(&*cfg, 1, &c.ctx, nullptr);
        if (st != ASTCENC_SUCCESS) return make_error(ErrorCode::InvalidArgument, astcenc_get_error_string(st));
        void* slice = img.pixels.data();
        astcenc_image out{width, height, 1, ASTCENC_TYPE_U8, &slice};
        st = astcenc_decompress_image(c.ctx, blocks.data(), blocks.size(), &out, &kIdentity, 0);
        if (st != ASTCENC_SUCCESS) return make_error(ErrorCode::InvalidArgument, astcenc_get_error_string(st));
        return img;
    }
    init_bc();
    const std::uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    const std::uint32_t block_bytes = codec == TextureCodec::BC1 ? 8 : 16;
    for (std::uint32_t by = 0; by < bh; ++by)
        for (std::uint32_t bx = 0; bx < bw; ++bx) {
            std::uint8_t px[64];
            const std::uint8_t* src = blocks.data() + (std::size_t{by} * bw + bx) * block_bytes;
            bool ok = true;
            switch (codec) {
            case TextureCodec::BC1: ok = rgbcx::unpack_bc1(src, px, true); break;
            case TextureCodec::BC3: ok = rgbcx::unpack_bc3(src, px); break;
            default: ok = bc7decomp::unpack_bc7(src, reinterpret_cast<bc7decomp::color_rgba*>(px)); break;
            }
            if (!ok && codec == TextureCodec::BC7) return make_error(ErrorCode::ParseError, "decode_texture: invalid BC7 block");
            for (std::uint32_t y = 0; y < 4 && by * 4 + y < height; ++y)
                for (std::uint32_t x = 0; x < 4 && bx * 4 + x < width; ++x)
                    std::memcpy(img.pixels.data() + ((std::size_t{by} * 4 + y) * width + bx * 4 + x) * 4, px + (y * 4 + x) * 4, 4);
        }
    return img;
}

double image_psnr(const render::Image& a, const render::Image& b, bool alpha) {
    if (a.width != b.width || a.height != b.height || a.pixels.size() != b.pixels.size() || a.pixels.empty()) return 0.0;
    double sum = 0.0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        if (!alpha && i % 4 == 3) continue;
        const double d = double(a.pixels[i]) - double(b.pixels[i]);
        sum += d * d;
        ++n;
    }
    if (sum == 0.0) return std::numeric_limits<double>::infinity();
    return 10.0 * std::log10(255.0 * 255.0 / (sum / double(n)));
}

Result<CompressedMips> compress_mips(std::span<const render::Image> mips, TextureCodec codec, bool srgb, CompressionEffort effort) {
    if (mips.empty()) return make_error(ErrorCode::InvalidArgument, "compress_mips: no mips");
    if (codec == TextureCodec::BC1) {
        const auto& p = mips[0].pixels;
        for (std::size_t i = 3; i < p.size(); i += 4)
            if (p[i] != 255) {
                codec = TextureCodec::BC3;
                break;
            }
    }
    CompressedMips out;
    out.codec = codec;
    out.levels.reserve(mips.size());
    for (const auto& m : mips) {
        auto enc = encode_texture(m, codec, srgb, effort);
        if (!enc) return std::move(enc).error();
        out.levels.push_back(std::move(*enc));
    }
    return out;
}

} // namespace victus::assets
