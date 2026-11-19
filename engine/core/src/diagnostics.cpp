// SPDX-License-Identifier: Apache-2.0
#include "victus/core/assert.hpp"
#include "victus/core/crash.hpp"
#include "victus/core/log.hpp"
#include "victus/core/result.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace victus {

const char* to_string(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Unknown: return "unknown";
    case ErrorCode::InvalidArgument: return "invalid_argument";
    case ErrorCode::NotFound: return "not_found";
    case ErrorCode::AlreadyExists: return "already_exists";
    case ErrorCode::IoError: return "io_error";
    case ErrorCode::ParseError: return "parse_error";
    case ErrorCode::Unsupported: return "unsupported";
    case ErrorCode::OutOfMemory: return "out_of_memory";
    case ErrorCode::LimitExceeded: return "limit_exceeded";
    case ErrorCode::DeviceError: return "device_error";
    case ErrorCode::VersionMismatch: return "version_mismatch";
    case ErrorCode::Cancelled: return "cancelled";
    case ErrorCode::ScriptError: return "script_error";
    case ErrorCode::NetworkError: return "network_error";
    case ErrorCode::Pending: return "pending";
    }
    return "?";
}

void assertion_failed(const char* expr, const char* message, const char* file, int line) noexcept {
    try {
        log::logger().write(log::Level::Fatal, "assert", std::string(file) + ":" + std::to_string(line) + ": " + expr + " — " + message);
        log::logger().flush();
    } catch (...) {
    }
    std::fprintf(stderr, "VICTUS assertion failed: %s (%s) at %s:%d\n", expr, message, file, line);
    std::abort();
}

void install_crash_handlers(const char* diagnostics_dir) { crash::install_handlers(fs::Path(diagnostics_dir), "victus"); }

} // namespace victus
