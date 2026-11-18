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

float alpha_coverage(const Image& image, float cutoff) noexcept {
    const std::size_t n = std::size_t{image.width} * image.height;
    if (n == 0 || image.pixels.size() < n * 4) return 0.0f;
    // Compare in 8-bit units: alpha a passes when a / 255 >= cutoff.
    const float ref = cutoff * 255.0f;
    std::size_t pass = 0;
    for (std::size_t i = 0; i < n; ++i) pass += static_cast<float>(image.pixels[i * 4 + 3]) >= ref ? 1u : 0u;
    return static_cast<float>(pass) / static_cast<float>(n);
}

void preserve_alpha_coverage(std::span<Image> chain, float cutoff) noexcept {
    if (chain.size() < 2 || !(cutoff > 0.0f && cutoff < 1.0f)) return;
    const float target = alpha_coverage(chain[0], cutoff);
    for (std::size_t m = 1; m < chain.size(); ++m) {
        Image& img = chain[m];
        // Bisect for the reference value r whose coverage on this level matches the base level
        // (coverage falls as r rises), then scale alpha by cutoff / r so r maps onto the cutoff.
        float lo = 0.0f, hi = 1.0f;
        for (int i = 0; i < 16; ++i) {
            const float mid = 0.5f * (lo + hi);
            if (alpha_coverage(img, mid) > target) lo = mid;
            else hi = mid;
        }
        const float r = std::max(0.5f * (lo + hi), 1.0f / 255.0f);
        const float scale = cutoff / r;
        if (std::abs(scale - 1.0f) < 1e-4f) continue;
        const std::size_t n = std::size_t{img.width} * img.height;
        for (std::size_t i = 0; i < n && i * 4 + 3 < img.pixels.size(); ++i) {
            const float a = static_cast<float>(img.pixels[i * 4 + 3]) * scale;
            img.pixels[i * 4 + 3] = static_cast<std::uint8_t>(std::lround(std::clamp(a, 0.0f, 255.0f)));
        }
    }
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

math::Vec4 ImageF::at(std::uint32_t x, std::uint32_t y) const noexcept {
    const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
    return {pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]};
}

bool is_hdr_image(std::span<const std::uint8_t> encoded) noexcept {
    return !encoded.empty() && stbi_is_hdr_from_memory(encoded.data(), static_cast<int>(encoded.size())) != 0;
}

Result<ImageF> decode_image_hdr(std::span<const std::uint8_t> encoded) {
    int w = 0, h = 0, comp = 0;
    float* data = stbi_loadf_from_memory(encoded.data(), static_cast<int>(encoded.size()), &w, &h, &comp, 4);
    if (!data) return make_error(ErrorCode::ParseError, std::format("hdr image decode failed: {}", stbi_failure_reason()));
    ImageF img;
    img.width = static_cast<std::uint32_t>(w);
    img.height = static_cast<std::uint32_t>(h);
    img.pixels.assign(data, data + static_cast<std::size_t>(w) * h * 4);
    stbi_image_free(data);
    // Untrusted input: NaN/Inf/negative texels would poison prefiltering and SH projection.
    for (float& f : img.pixels)
        if (!std::isfinite(f) || f < 0.0f) f = 0.0f;
        else f = std::min(f, 65000.0f);
    return img;
}

Result<ImageF> load_image_hdr(const fs::Path& path) {
    auto bytes = fs::read_binary(path);
    if (!bytes) return std::move(bytes).error();
    auto img = decode_image_hdr(bytes.value());
    if (!img) return make_error(ErrorCode::ParseError, std::format("{}: {}", path.string(), img.error().message));
    return img;
}

Result<std::vector<std::uint8_t>> encode_hdr(const ImageF& image) {
    if (image.width == 0 || image.height == 0 || image.pixels.size() < std::size_t{image.width} * image.height * 4)
        return make_error(ErrorCode::InvalidArgument, "encode_hdr: invalid image");
    std::vector<std::uint8_t> out;
    auto writer = [](void* ctx, void* data, int size) {
        auto* v = static_cast<std::vector<std::uint8_t>*>(ctx);
        auto* p = static_cast<std::uint8_t*>(data);
        v->insert(v->end(), p, p + size);
    };
    if (!stbi_write_hdr_to_func(writer, &out, static_cast<int>(image.width), static_cast<int>(image.height), 4,
                                image.pixels.data()))
        return make_error(ErrorCode::IoError, "hdr encode failed");
    return out;
}

ImageF downsample_to_fit(const ImageF& image, std::uint32_t max_width, std::uint32_t max_height) {
    ImageF cur = image;
    while ((cur.width > max_width && cur.width > 1) || (cur.height > max_height && cur.height > 1)) {
        const std::uint32_t fx = cur.width > max_width && cur.width > 1 ? 2u : 1u;
        const std::uint32_t fy = cur.height > max_height && cur.height > 1 ? 2u : 1u;
        ImageF next;
        next.width = std::max(1u, cur.width / fx);
        next.height = std::max(1u, cur.height / fy);
        next.pixels.assign(std::size_t{next.width} * next.height * 4, 0.0f);
        for (std::uint32_t y = 0; y < next.height; ++y)
            for (std::uint32_t x = 0; x < next.width; ++x) {
                float acc[4] = {0, 0, 0, 0};
                for (std::uint32_t dy = 0; dy < fy; ++dy)
                    for (std::uint32_t dx = 0; dx < fx; ++dx) {
                        const std::uint32_t sx = std::min(x * fx + dx, cur.width - 1), sy = std::min(y * fy + dy, cur.height - 1);
                        const std::size_t si = (std::size_t{sy} * cur.width + sx) * 4;
                        for (int c = 0; c < 4; ++c) acc[c] += cur.pixels[si + c];
                    }
                const float inv = 1.0f / static_cast<float>(fx * fy);
                const std::size_t di = (std::size_t{y} * next.width + x) * 4;
                for (int c = 0; c < 4; ++c) next.pixels[di + c] = acc[c] * inv;
            }
        cur = std::move(next);
    }
    return cur;
}

std::uint16_t float_to_half(float f) noexcept {
    std::uint32_t x;
    std::memcpy(&x, &f, 4);
    const std::uint32_t sign = (x >> 16) & 0x8000u;
    std::uint32_t mag = x & 0x7fffffffu;
    if (mag > 0x7f800000u) return static_cast<std::uint16_t>(sign | 0x7e00u); // NaN
    if (mag >= 0x477ff000u) return static_cast<std::uint16_t>(sign | 0x7bffu); // clamp to max finite (65504)
    if (mag < 0x38800000u) {                                                    // subnormal half or zero
        if (mag < 0x33000000u) return static_cast<std::uint16_t>(sign);
        const std::uint32_t e = mag >> 23;
        std::uint32_t m = (mag & 0x7fffffu) | 0x800000u;
        const std::uint32_t shift = 126u - e; // 14..24
        std::uint32_t h = m >> shift;
        const std::uint32_t rem = m & ((1u << shift) - 1u), half = 1u << (shift - 1u);
        if (rem > half || (rem == half && (h & 1u))) ++h;
        return static_cast<std::uint16_t>(sign | h);
    }
    std::uint32_t h = ((mag - 0x38000000u) >> 13);
    const std::uint32_t rem = mag & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) ++h;
    return static_cast<std::uint16_t>(sign | std::min(h, 0x7bffu));
}

float half_to_float(std::uint16_t h) noexcept {
    const std::uint32_t sign = (std::uint32_t{h} & 0x8000u) << 16;
    const std::uint32_t e = (h >> 10) & 0x1fu, m = h & 0x3ffu;
    std::uint32_t x;
    if (e == 0) {
        if (m == 0) x = sign;
        else {
            const float v = std::ldexp(static_cast<float>(m), -24);
            std::memcpy(&x, &v, 4);
            x |= sign;
        }
    } else if (e == 31) x = sign | 0x7f800000u | (m << 13);
    else x = sign | ((e + 112u) << 23) | (m << 13);
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}

std::vector<std::uint8_t> to_rgba16f(const ImageF& image) {
    std::vector<std::uint8_t> out(image.pixels.size() * 2);
    for (std::size_t i = 0; i < image.pixels.size(); ++i) {
        const std::uint16_t hv = float_to_half(image.pixels[i]);
        std::memcpy(out.data() + i * 2, &hv, 2);
    }
    return out;
}

} // namespace victus::render
