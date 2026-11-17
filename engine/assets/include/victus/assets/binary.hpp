// SPDX-License-Identifier: Apache-2.0
// Bounds-checked little-endian binary serialization for cooked assets. Every read validates
// remaining size so a truncated or malicious cache file yields an error, never UB.
#pragma once
#include "victus/core/result.hpp"

#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace victus::assets {

static_assert(std::endian::native == std::endian::little, "VICTUS cooked formats assume little-endian hosts");

class BinaryWriter {
public:
    template <typename T>
    void pod(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
        bytes_.insert(bytes_.end(), p, p + sizeof(T));
    }
    void u8(std::uint8_t v) { pod(v); }
    void u32(std::uint32_t v) { pod(v); }
    void i32(std::int32_t v) { pod(v); }
    void u64(std::uint64_t v) { pod(v); }
    void f32(float v) { pod(v); }
    void str(std::string_view s) {
        u32(static_cast<std::uint32_t>(s.size()));
        bytes_.insert(bytes_.end(), s.begin(), s.end());
    }
    void raw(std::span<const std::uint8_t> data) {
        u64(data.size());
        bytes_.insert(bytes_.end(), data.begin(), data.end());
    }
    template <typename T>
    void array(const std::vector<T>& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        u64(v.size());
        const auto* p = reinterpret_cast<const std::uint8_t*>(v.data());
        bytes_.insert(bytes_.end(), p, p + v.size() * sizeof(T));
    }
    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::vector<std::uint8_t>&& take() noexcept { return std::move(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
};

class BinaryReader {
public:
    explicit BinaryReader(std::span<const std::uint8_t> data) : data_(data) {}
    template <typename T>
    bool pod(T& out) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (!need(sizeof(T))) return false;
        std::memcpy(&out, data_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return true;
    }
    bool u8(std::uint8_t& v) { return pod(v); }
    bool u32(std::uint32_t& v) { return pod(v); }
    bool i32(std::int32_t& v) { return pod(v); }
    bool u64(std::uint64_t& v) { return pod(v); }
    bool f32(float& v) { return pod(v); }
    bool boolean(bool& v) {
        std::uint8_t b = 0;
        if (!u8(b) || b > 1) return fail();
        v = b != 0;
        return true;
    }
    bool str(std::string& out, std::uint32_t max_len = 1u << 20) {
        std::uint32_t n = 0;
        if (!u32(n) || n > max_len || !need(n)) return fail();
        out.assign(reinterpret_cast<const char*>(data_.data() + pos_), n);
        pos_ += n;
        return true;
    }
    bool raw(std::vector<std::uint8_t>& out, std::uint64_t max_len = 1ull << 32) {
        std::uint64_t n = 0;
        if (!u64(n) || n > max_len || !need(n)) return fail();
        out.assign(data_.begin() + static_cast<std::ptrdiff_t>(pos_), data_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
        pos_ += n;
        return true;
    }
    template <typename T>
    bool array(std::vector<T>& out, std::uint64_t max_count = 1ull << 28) {
        static_assert(std::is_trivially_copyable_v<T>);
        std::uint64_t n = 0;
        if (!u64(n) || n > max_count || !need(n * sizeof(T))) return fail();
        out.resize(n);
        if (n != 0) std::memcpy(out.data(), data_.data() + pos_, n * sizeof(T)); // empty vectors may have no storage
        pos_ += n * sizeof(T);
        return true;
    }
    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] bool at_end() const noexcept { return pos_ == data_.size(); }
    [[nodiscard]] std::size_t position() const noexcept { return pos_; }

private:
    bool need(std::uint64_t n) {
        if (!ok_ || n > data_.size() - pos_) return fail();
        return true;
    }
    bool fail() {
        ok_ = false;
        return false;
    }
    std::span<const std::uint8_t> data_;
    std::size_t pos_{0};
    bool ok_{true};
};

} // namespace victus::assets
