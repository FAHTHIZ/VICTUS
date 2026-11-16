// SPDX-License-Identifier: Apache-2.0
#include "victus/render/image.hpp"

#include "victus/core/math.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include "stb_image.h"
#include "stb_image_write.h"

namespace victus::render {

std::uint32_t Image::pixel(std::uint32_t x, std::uint32_t y) const noexcept {
    const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
    std::uint32_t v;
    std::memcpy(&v, pixels.data() + i, 4);
    return v;
}

Result<Image> decode_image(std::span<const std::uint8_t> encoded) {
    int w = 0, h = 0, comp = 0;
    stbi_uc* data = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &w, &h, &comp, 4);
    if (!data) return make_error(ErrorCode::ParseError, std::format("image decode failed: {}", stbi_failure_reason()));
    Image img;
    img.width = static_cast<std::uint32_t>(w);
    img.height = static_cast<std::uint32_t>(h);
    img.pixels.assign(data, data + static_cast<std::size_t>(w) * h * 4);
    stbi_image_free(data);
    return img;
}

Result<Image> load_image(const fs::Path& path) {
    auto bytes = fs::read_binary(path);
    if (!bytes) return std::move(bytes).error();
    auto img = decode_image(bytes.value());
    if (!img) return make_error(ErrorCode::ParseError, std::format("{}: {}", path.string(), img.error().message));
    return img;
}

Result<std::vector<std::uint8_t>> encode_png(const Image& image) {
    if (image.width == 0 || image.height == 0 || image.pixels.size() < std::size_t{image.width} * image.height * 4)
        return make_error(ErrorCode::InvalidArgument, "encode_png: invalid image");
    std::vector<std::uint8_t> out;
    auto writer = [](void* ctx, void* data, int size) {
        auto* v = static_cast<std::vector<std::uint8_t>*>(ctx);
        auto* p = static_cast<std::uint8_t*>(data);
        v->insert(v->end(), p, p + size);
    };
    if (!stbi_write_png_to_func(writer, &out, static_cast<int>(image.width), static_cast<int>(image.height), 4,
                                image.pixels.data(), static_cast<int>(image.width * 4)))
        return make_error(ErrorCode::IoError, "png encode failed");
    return out;
}

Status save_png(const fs::Path& path, const Image& image) {
    auto png = encode_png(image);
    if (!png) return std::move(png).error();
    return fs::write_atomic(path, png.value());
}

std::uint32_t mip_count(std::uint32_t width, std::uint32_t height) noexcept {
    std::uint32_t n = 1, s = std::max(width, height);
    while (s > 1) {
        s >>= 1;
        ++n;
    }
    return n;
}

std::vector<Image> build_mip_chain(const Image& base, bool srgb) {
    std::vector<Image> chain{base};
    float lut[256];
    for (int i = 0; i < 256; ++i) lut[i] = srgb ? math::srgb_to_linear(static_cast<float>(i) / 255.0f) : i / 255.0f;
    auto encode = [&](float v) {
        v = std::clamp(v, 0.0f, 1.0f);
        if (srgb) v = math::linear_to_srgb(v);
        return static_cast<std::uint8_t>(std::lround(v * 255.0f));
    };
    while (chain.back().width > 1 || chain.back().height > 1) {
        const Image& src = chain.back();
        Image dst;
        dst.width = std::max(1u, src.width / 2);
        dst.height = std::max(1u, src.height / 2);
        dst.pixels.resize(std::size_t{dst.width} * dst.height * 4);
        for (std::uint32_t y = 0; y < dst.height; ++y) {
            for (std::uint32_t x = 0; x < dst.width; ++x) {
                for (int c = 0; c < 4; ++c) {
                    float sum = 0;
                    for (std::uint32_t dy = 0; dy < 2; ++dy) {
                        for (std::uint32_t dx = 0; dx < 2; ++dx) {
                            std::uint32_t sx = std::min(src.width - 1, x * 2 + dx);
                            std::uint32_t sy = std::min(src.height - 1, y * 2 + dy);
                            std::uint8_t v = src.pixels[(std::size_t{sy} * src.width + sx) * 4 + c];
                            sum += c == 3 ? v / 255.0f : lut[v];
                        }
                    }
                    sum *= 0.25f;
                    dst.pixels[(std::size_t{y} * dst.width + x) * 4 + c] =
                        c == 3 ? static_cast<std::uint8_t>(std::lround(std::clamp(sum, 0.0f, 1.0f) * 255.0f)) : encode(sum);
                }
            }
        }
        chain.push_back(std::move(dst));
    }
    return chain;
}

Image make_checker(std::uint32_t size, std::uint32_t cells, std::uint32_t a, std::uint32_t b) {
    Image img;
    img.width = img.height = size;
    img.pixels.resize(std::size_t{size} * size * 4);
    const std::uint32_t cell = std::max(1u, size / std::max(1u, cells));
    for (std::uint32_t y = 0; y < size; ++y)
        for (std::uint32_t x = 0; x < size; ++x) {
            std::uint32_t v = ((x / cell) + (y / cell)) % 2 ? b : a;
            std::memcpy(img.pixels.data() + (std::size_t{y} * size + x) * 4, &v, 4);
        }
    return img;
}

} // namespace victus::render
