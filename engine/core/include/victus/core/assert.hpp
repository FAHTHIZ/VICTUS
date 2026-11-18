// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace victus {
// Logs the failure with source location, flushes logs, then aborts.
[[noreturn]] void assertion_failed(const char* expr, const char* message, const char* file, int line) noexcept;
// Installs handlers for fatal signals / unhandled exceptions that write a crash report into
// `diagnostics_dir`/crashes (see victus/core/crash.hpp; crash::Session also adds session logs).
void install_crash_handlers(const char* diagnostics_dir);
} // namespace victus

#define VICTUS_VERIFY(expr, msg)                                                                   \
    do {                                                                                           \
        if (!(expr)) ::victus::assertion_failed(#expr, msg, __FILE__, __LINE__);                  \
    } while (0)

#if defined(NDEBUG) && !defined(VICTUS_FORCE_ASSERTS)
#define VICTUS_ASSERT(expr, msg) ((void)0)
#else
#define VICTUS_ASSERT(expr, msg) VICTUS_VERIFY(expr, msg)
#endif
