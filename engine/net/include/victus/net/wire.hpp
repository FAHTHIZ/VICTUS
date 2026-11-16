// SPDX-License-Identifier: Apache-2.0
// Bounds-checked little-endian binary serialization for network payloads. Readers never read
// past the end: a short or malformed buffer sets failed() and returns zero values, so message
// handlers validate once at the end instead of after every field.
#pragma once
#include "victus/core/math.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace victus::net {

class ByteWriter {
public:
    void u8(std::uint8_t v) { buf_.push_back(v); }
    void u16(std::uint16_t v) { raw(&v, 2); }
    void u32(std::uint32_t v) { raw(&v, 4); }
    void u64(std::uint64_t v) { raw(&v, 8); }
    void f32(float v) { raw(&v, 4); }
    void f64(double v) { raw(&v, 8); }
    void varuint(std::uint64_t v) {
        while (v >= 0x80) {
            buf_.push_back(static_cast<std::uint8_t>(v | 0x80));
            v >>= 7;
        }
        buf_.push_back(static_cast<std::uint8_t>(v));
    }
    void str(std::string_view s) {
        varuint(s.size());
        buf_.insert(buf_.end(), s.begin(), s.end());
    }
    void bytes(std::span<const std::uint8_t> b) { buf_.insert(buf_.end(), b.begin(), b.end()); }
    void vec3(math::Vec3 v) {
        f32(v.x);
        f32(v.y);
        f32(v.z);
    }
    // Unit quaternion, 16 bits per component (max error ~3e-5).
    void quat(math::Quat q) {
        if (q.w < 0) q = {-q.x, -q.y, -q.z, -q.w};
        for (const float c : {q.x, q.y, q.z, q.w}) {
            const float k = c < -1.0f ? -1.0f : (c > 1.0f ? 1.0f : c);
            u16(static_cast<std::uint16_t>(static_cast<std::int32_t>(k * 32767.0f) + 32768));
        }
    }
    [[nodiscard]] std::size_t size() const noexcept { return buf_.size(); }
    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return buf_; }
    [[nodiscard]] std::vector<std::uint8_t> take() noexcept { return std::move(buf_); }
    void truncate(std::size_t n) { buf_.resize(n); }
    void patch_u16(std::size_t at, std::uint16_t v) { std::memcpy(buf_.data() + at, &v, 2); }

private:
    void raw(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n); // little-endian hosts only (checked in net.cpp)
    }
    std::vector<std::uint8_t> buf_;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> data) : data_(data) {}
    std::uint8_t u8() {
        std::uint8_t v = 0;
        raw(&v, 1);
        return v;
    }
    std::uint16_t u16() {
        std::uint16_t v = 0;
        raw(&v, 2);
        return v;
    }
    std::uint32_t u32() {
        std::uint32_t v = 0;
        raw(&v, 4);
        return v;
    }
    std::uint64_t u64() {
        std::uint64_t v = 0;
        raw(&v, 8);
        return v;
    }
    float f32() {
        float v = 0;
        raw(&v, 4);
        return finite(v);
    }
    double f64() {
        double v = 0;
        raw(&v, 8);
        if (!(v == v) || v > 1e300 || v < -1e300) return fail(), 0.0;
        return v;
    }
    std::uint64_t varuint() {
        std::uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            const std::uint8_t b = u8();
            if (failed_) return 0;
            v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
        }
        return fail(), 0;
    }
    // Strings longer than max_len fail the reader (untrusted input).
    std::string str(std::size_t max_len) {
        const std::uint64_t n = varuint();
        if (failed_ || n > max_len || n > remaining()) return fail(), std::string{};
        std::string s(reinterpret_cast<const char*>(data_.data() + pos_), static_cast<std::size_t>(n));
        pos_ += static_cast<std::size_t>(n);
        return s;
    }
    math::Vec3 vec3() {
        const float x = f32(), y = f32(), z = f32();
        return {x, y, z};
    }
    math::Quat quat() {
        float c[4];
        for (float& v : c) v = (static_cast<float>(u16()) - 32768.0f) / 32767.0f;
        math::Quat q{c[0], c[1], c[2], c[3]};
        const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (len < 1e-3f) return {};
        return {q.x / len, q.y / len, q.z / len, q.w / len};
    }
    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - pos_; }
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] bool ok() const noexcept { return !failed_; }
    // True when everything was consumed without error (trailing garbage is a violation).
    [[nodiscard]] bool done() const noexcept { return !failed_ && pos_ == data_.size(); }
    void fail() noexcept { failed_ = true; }

private:
    void raw(void* p, std::size_t n) {
        if (failed_ || n > remaining()) {
            failed_ = true;
            std::memset(p, 0, n);
            return;
        }
        std::memcpy(p, data_.data() + pos_, n);
        pos_ += n;
    }
    float finite(float v) {
        if (!(v == v) || v > 1e30f || v < -1e30f) return fail(), 0.0f;
        return v;
    }
    std::span<const std::uint8_t> data_;
    std::size_t pos_{0};
    bool failed_{false};
};

} // namespace victus::net
