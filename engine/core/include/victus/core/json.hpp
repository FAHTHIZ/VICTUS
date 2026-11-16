// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "victus/core/result.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace victus::json {

class Value;
using Array = std::vector<Value>;
// Objects preserve insertion order so serialized files are deterministic and diff-friendly.
using Object = std::vector<std::pair<std::string, Value>>;

enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    Value(int v) : type_(Type::Number), number_(v) {}
    Value(std::int64_t v) : type_(Type::Number), number_(static_cast<double>(v)) {}
    Value(std::uint64_t v) : type_(Type::Number), number_(static_cast<double>(v)) {}
    Value(std::uint32_t v) : type_(Type::Number), number_(v) {}
    Value(double v) : type_(Type::Number), number_(v) {}
    Value(float v) : type_(Type::Number), number_(v) {}
    Value(const char* s) : type_(Type::String), string_(std::make_shared<std::string>(s)) {}
    Value(std::string s) : type_(Type::String), string_(std::make_shared<std::string>(std::move(s))) {}
    Value(std::string_view s) : type_(Type::String), string_(std::make_shared<std::string>(s)) {}
    Value(Array a) : type_(Type::Array), array_(std::make_shared<Array>(std::move(a))) {}
    Value(Object o) : type_(Type::Object), object_(std::make_shared<Object>(std::move(o))) {}

    Value(const Value& other) { *this = other; }
    Value& operator=(const Value& other);
    Value(Value&&) noexcept = default;
    Value& operator=(Value&&) noexcept = default;

    static Value array() { return Value(Array{}); }
    static Value object() { return Value(Object{}); }

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool is_null() const noexcept { return type_ == Type::Null; }
    [[nodiscard]] bool is_bool() const noexcept { return type_ == Type::Bool; }
    [[nodiscard]] bool is_number() const noexcept { return type_ == Type::Number; }
    [[nodiscard]] bool is_string() const noexcept { return type_ == Type::String; }
    [[nodiscard]] bool is_array() const noexcept { return type_ == Type::Array; }
    [[nodiscard]] bool is_object() const noexcept { return type_ == Type::Object; }

    [[nodiscard]] bool as_bool(bool fallback = false) const noexcept { return is_bool() ? bool_ : fallback; }
    [[nodiscard]] double as_number(double fallback = 0.0) const noexcept { return is_number() ? number_ : fallback; }
    [[nodiscard]] float as_float(float fallback = 0.0f) const noexcept {
        return is_number() ? static_cast<float>(number_) : fallback;
    }
    [[nodiscard]] std::int64_t as_int(std::int64_t fallback = 0) const noexcept {
        return is_number() ? static_cast<std::int64_t>(number_) : fallback;
    }
    [[nodiscard]] const std::string& as_string() const noexcept;
    [[nodiscard]] std::string as_string_or(std::string_view fallback) const {
        return is_string() ? *string_ : std::string(fallback);
    }

    [[nodiscard]] const Array& as_array() const noexcept;
    [[nodiscard]] Array& as_array();
    [[nodiscard]] const Object& as_object() const noexcept;
    [[nodiscard]] Object& as_object();

    // Object access. find() returns nullptr when missing or not an object.
    [[nodiscard]] const Value* find(std::string_view key) const noexcept;
    [[nodiscard]] const Value& operator[](std::string_view key) const noexcept;
    // Inserts or replaces; converts a null value into an object.
    Value& set(std::string_view key, Value value);
    // Array access.
    [[nodiscard]] const Value& operator[](std::size_t index) const noexcept;
    Value& push(Value value);
    [[nodiscard]] std::size_t size() const noexcept;

    friend bool operator==(const Value& a, const Value& b);

private:
    void ensure_unique();
    Type type_{Type::Null};
    bool bool_{false};
    double number_{0.0};
    std::shared_ptr<std::string> string_;
    std::shared_ptr<Array> array_;
    std::shared_ptr<Object> object_;
};

struct ParseLimits {
    std::size_t max_depth{256};
    std::size_t max_bytes{256u * 1024u * 1024u};
    std::size_t max_string_bytes{64u * 1024u * 1024u};
};

[[nodiscard]] Result<Value> parse(std::string_view text, const ParseLimits& limits = {});
[[nodiscard]] std::string stringify(const Value& value, bool pretty = true);

} // namespace victus::json
