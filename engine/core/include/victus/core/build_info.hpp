// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace victus::build {
const char* version() noexcept;     // semantic version, e.g. "0.2.0"
const char* git_revision() noexcept; // short commit SHA at configure time, or "unknown"
const char* compiler() noexcept;
const char* platform() noexcept;     // "windows", "linux", "macos", "android", "ios"
const char* architecture() noexcept; // "x86_64", "arm64", ...
const char* build_type() noexcept;   // CMake configuration
} // namespace victus::build
