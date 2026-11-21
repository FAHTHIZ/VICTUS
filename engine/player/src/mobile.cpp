// SPDX-License-Identifier: Apache-2.0
#include "victus/player/mobile.hpp"

#include "victus/core/log.hpp"
#include "victus/core/pack.hpp"
#include "victus/player/export.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <vector>

namespace victus::player {

namespace {
// The v2 header ends with the TOC digest, so these bytes change whenever any entry does.
constexpr std::size_t kHeaderBytes = pack::kPackIdentityBytes;

std::vector<std::uint8_t> installed_header(const fs::Path& path, std::uint64_t* size) {
    std::error_code ec;
    *size = 0;
    if (!std::filesystem::is_regular_file(path, ec)) return {};
    *size = std::filesystem::file_size(path, ec);
    if (ec) return {};
    std::ifstream f(path, std::ios::binary);
    std::vector<std::uint8_t> h(kHeaderBytes);
    f.read(reinterpret_cast<char*>(h.data()), static_cast<std::streamsize>(h.size()));
    h.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, f.gcount())));
    return h;
}
} // namespace

Result<BundledInstall> install_bundled_pack(const std::string& bundled, const fs::Path& dest_dir) {
    std::uint64_t bundled_size = 0;
    auto head = platform::Platform::read_bundled_prefix(bundled, kHeaderBytes, &bundled_size);
    if (!head) return std::move(head).error();
    if (head->size() < kHeaderBytes || std::memcmp(head->data(), "VPAK", 4) != 0)
        return make_error(ErrorCode::InvalidArgument, std::format("bundled '{}' is not a VICTUS pack", bundled));

    BundledInstall out;
    out.game_dir = dest_dir;
    out.bytes = bundled_size;
    const fs::Path target = dest_dir / std::string(kPackFile);
    std::uint64_t installed_size = 0;
    if (installed_header(target, &installed_size) == *head && installed_size == bundled_size) return out;

    auto copied = platform::Platform::copy_bundled(bundled, target.string());
    if (!copied) return std::move(copied).error();
    if (*copied != bundled_size)
        return make_error(ErrorCode::IoError, std::format("bundled '{}' changed while copying", bundled));
    out.copied = true;
    VICTUS_LOG_INFO("player", "installed bundled game pack ({} bytes) to {}", bundled_size, target.string());
    return out;
}

} // namespace victus::player
