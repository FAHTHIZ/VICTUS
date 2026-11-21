// SPDX-License-Identifier: Apache-2.0
// Cryptographically secure random bytes from the operating system (BCryptGenRandom on Windows,
// arc4random_buf on Apple platforms, getrandom elsewhere). Used for session keys and signing keys;
// never use PCG32 or std::random_device for secrets.
#pragma once
#include "victus/core/result.hpp"

#include <cstdint>
#include <span>

namespace victus {

[[nodiscard]] Status os_random_bytes(std::span<std::uint8_t> out);

} // namespace victus
