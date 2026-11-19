// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace victus {

enum class ErrorCode : std::uint16_t {
    Unknown = 1,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    IoError,
    ParseError,
    Unsupported,
    OutOfMemory,
    LimitExceeded,
    DeviceError,
    VersionMismatch,
    Cancelled,
    ScriptError,
    NetworkError,
    Pending, // the resource is loading asynchronously; ask again on a later frame
};

[[nodiscard]] const char* to_string(ErrorCode code) noexcept;

struct Error {
    ErrorCode code{ErrorCode::Unknown};
    std::string message;
};

[[nodiscard]] inline Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

// Minimal C++20 expected-like type; VICTUS keeps a C++20 baseline across all
// supported toolchains (see ADR-0001), so std::expected is not used.
template <typename T>
class [[nodiscard]] Result {
public:
    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] T& value() & { return std::get<0>(storage_); }
    [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }
    [[nodiscard]] T* operator->() { return &value(); }
    [[nodiscard]] const T* operator->() const { return &value(); }
    [[nodiscard]] T& operator*() & { return value(); }
    [[nodiscard]] const T& operator*() const& { return value(); }

    [[nodiscard]] const Error& error() const& { return std::get<1>(storage_); }
    [[nodiscard]] Error&& error() && { return std::get<1>(std::move(storage_)); }

    template <typename U>
    [[nodiscard]] T value_or(U&& fallback) const& {
        return ok() ? value() : static_cast<T>(std::forward<U>(fallback));
    }

private:
    std::variant<T, Error> storage_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)), ok_(false) {}
    [[nodiscard]] bool ok() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }
    [[nodiscard]] const Error& error() const& { return error_; }
    [[nodiscard]] Error&& error() && { return std::move(error_); }

private:
    Error error_{};
    bool ok_{true};
};

using Status = Result<void>;

} // namespace victus

#define VICTUS_TRY_STATUS(expr)                                                                    \
    do {                                                                                           \
        auto victus_try_status_ = (expr);                                                          \
        if (!victus_try_status_.ok()) return std::move(victus_try_status_).error();                \
    } while (0)
