// SPDX-License-Identifier: Apache-2.0
#include "victus/core/os_random.hpp"

#include <cerrno>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#    include <bcrypt.h>
#elif defined(__APPLE__)
#    include <stdlib.h>
#else
#    include <sys/random.h>
#endif

namespace victus {

Status os_random_bytes(std::span<std::uint8_t> out) {
#if defined(_WIN32)
    if (BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return make_error(ErrorCode::IoError, "BCryptGenRandom failed");
#elif defined(__APPLE__)
    arc4random_buf(out.data(), out.size());
#else
    std::size_t done = 0;
    while (done < out.size()) {
        const ssize_t n = getrandom(out.data() + done, out.size() - done, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return make_error(ErrorCode::IoError, "getrandom failed");
        }
        done += static_cast<std::size_t>(n);
    }
#endif
    return {};
}

} // namespace victus
