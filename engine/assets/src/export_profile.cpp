// SPDX-License-Identifier: Apache-2.0
#include "victus/assets/export_profile.hpp"

#include <algorithm>
#include <format>

namespace victus::assets {

Result<TextureExportProfile> texture_export_profile(std::string_view target) {
    TextureExportProfile p;
    p.target = std::string(target.empty() ? "all" : target);
    if (p.target == "all") return p;
    if (p.target == "desktop") {
        p.drop_mobile_codecs = true;
        return p;
    }
    if (p.target == "android") {
        // Phones render at 720p-1440p (and below with dynamic resolution): 1024 px is the
        // largest level a phone screen resolves on a character or prop.
        p.max_size = 1024;
        p.drop_desktop_codecs = true;
        p.rgba8_max_size = 64;
        return p;
    }
    return make_error(ErrorCode::InvalidArgument, std::format("unknown export target '{}' (all, desktop, android)", target));
}

void apply_texture_profile(CookedTexture& t, const TextureExportProfile& p, TextureProfileStats* stats) {
    if (t.hdr || t.mips.empty()) return;
    if (stats) ++stats->textures;
    // Variants whose level count does not match are never used by the runtime: drop them.
    const std::size_t before = t.compressed.size();
    std::erase_if(t.compressed, [&](const CompressedMips& v) {
        if (v.levels.size() != t.mips.size()) return true;
        const bool desktop = is_desktop_codec(v.codec);
        return desktop ? p.drop_desktop_codecs : p.drop_mobile_codecs;
    });
    if (stats) stats->variants_dropped += before - t.compressed.size();
    if (p.max_size > 0) {
        std::size_t drop = 0;
        while (drop + 1 < t.mips.size() && std::max(t.mips[drop].width, t.mips[drop].height) > p.max_size) ++drop;
        if (drop > 0) {
            t.mips.erase(t.mips.begin(), t.mips.begin() + static_cast<std::ptrdiff_t>(drop));
            for (auto& v : t.compressed) v.levels.erase(v.levels.begin(), v.levels.begin() + static_cast<std::ptrdiff_t>(drop));
            if (stats) stats->levels_dropped += drop;
        }
    }
    if (p.rgba8_max_size > 0 && !t.compressed.empty()) {
        for (auto& m : t.mips) {
            if (std::max(m.width, m.height) <= p.rgba8_max_size || m.pixels.empty()) continue;
            m.pixels.clear();
            m.pixels.shrink_to_fit();
            if (stats) ++stats->rgba8_stripped;
        }
    }
}

void apply_texture_profile(CookedModel& m, const TextureExportProfile& p, TextureProfileStats* stats) {
    for (auto& t : m.textures) apply_texture_profile(t, p, stats);
}

Result<std::vector<std::uint8_t>> apply_texture_profile(std::span<const std::uint8_t> bytes, const TextureExportProfile& p,
                                                        TextureProfileStats* stats) {
    if (p.target == "all") return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    auto header = read_header(bytes);
    if (!header) return std::move(header).error();
    if (header->kind == AssetKind::Texture) {
        auto t = deserialize_texture(bytes);
        if (!t) return std::move(t).error();
        apply_texture_profile(*t, p, stats);
        return serialize(*header, *t);
    }
    if (header->kind == AssetKind::Model) {
        auto m = deserialize_model(bytes);
        if (!m) return std::move(m).error();
        apply_texture_profile(*m, p, stats);
        return serialize(*header, *m);
    }
    return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

} // namespace victus::assets
