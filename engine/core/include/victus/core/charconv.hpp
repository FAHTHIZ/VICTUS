// SPDX-License-Identifier: Apache-2.0
// Locale-independent floating-point parsing. std::from_chars for double is missing from the libc++ of the Android
// NDK (r27) and older Apple SDKs, so VICTUS parses through this helper on every platform.
#pragma once
#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <system_error>

namespace victus {

// Same contract as std::from_chars(first, last, value) for double (general format): parses the longest valid
// prefix, `ptr` points past it, `ec` is invalid_argument when nothing parsed and result_out_of_range on overflow.
inline std::from_chars_result from_chars_double(const char* first, const char* last, double& value) noexcept {
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    return std::from_chars(first, last, value);
#else
    // strtod needs a terminated string and must not see a hex prefix, "inf"/"nan" or leading whitespace, which
    // from_chars rejects. Bionic and Apple use the "C" numeric locale for strtod in apps that never call setlocale.
    char buf[128];
    const std::size_t n = static_cast<std::size_t>(last - first);
    if (n == 0 || n >= sizeof buf) return {first, std::errc::invalid_argument};
    std::memcpy(buf, first, n);
    buf[n] = '\0';
    const char* s = buf[0] == '-' ? buf + 1 : buf;
    if (!((*s >= '0' && *s <= '9') || (*s == '.' && s[1] >= '0' && s[1] <= '9'))) return {first, std::errc::invalid_argument};
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        value = buf[0] == '-' ? -0.0 : 0.0;
        return {first + (s - buf) + 1, std::errc{}};
    }
    char* end = nullptr;
    errno = 0;
    const double d = std::strtod(buf, &end);
    const char* ptr = first + (end - buf);
    if (errno == ERANGE && (d > 1.0 || d < -1.0)) return {ptr, std::errc::result_out_of_range};
    value = d;
    return {ptr, std::errc{}};
#endif
}

} // namespace victus
