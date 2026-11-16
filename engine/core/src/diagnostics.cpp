// SPDX-License-Identifier: Apache-2.0
#include "victus/core/assert.hpp"
#include "victus/core/build_info.hpp"
#include "victus/core/log.hpp"
#include "victus/core/result.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
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
    }
    return "?";
}

namespace {
char g_crash_path[1024] = {0};

void write_crash_context(const char* reason) noexcept {
    if (!g_crash_path[0]) return;
    if (std::FILE* f = std::fopen(g_crash_path, "wb")) {
        std::fprintf(f, "VICTUS crash context\nreason: %s\nversion: %s (%s)\nplatform: %s %s\ncompiler: %s\nbuild: %s\n",
                     reason, build::version(), build::git_revision(), build::platform(), build::architecture(),
                     build::compiler(), build::build_type());
        std::fclose(f);
    }
}

extern "C" void victus_signal_handler(int sig) {
    const char* name = sig == SIGSEGV ? "SIGSEGV" : sig == SIGABRT ? "SIGABRT" : sig == SIGFPE ? "SIGFPE" : sig == SIGILL ? "SIGILL" : "signal";
    // Best effort: this is not fully async-signal-safe, but the process is terminating.
    write_crash_context(name);
    std::fprintf(stderr, "VICTUS fatal %s; crash context written to %s\n", name, g_crash_path);
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
} // namespace

void assertion_failed(const char* expr, const char* message, const char* file, int line) noexcept {
    try {
        log::logger().write(log::Level::Fatal, "assert", std::string(file) + ":" + std::to_string(line) + ": " + expr + " — " + message);
        log::logger().flush();
    } catch (...) {
    }
    std::fprintf(stderr, "VICTUS assertion failed: %s (%s) at %s:%d\n", expr, message, file, line);
    std::abort();
}

void install_crash_handlers(const char* diagnostics_dir) {
    std::snprintf(g_crash_path, sizeof g_crash_path, "%s/crash_context.txt", diagnostics_dir);
    for (int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL}) std::signal(sig, victus_signal_handler);
    std::set_terminate([] {
        write_crash_context("std::terminate (unhandled exception)");
        log::logger().flush();
        std::fprintf(stderr, "VICTUS terminated by unhandled exception\n");
        std::_Exit(3);
    });
}

} // namespace victus
