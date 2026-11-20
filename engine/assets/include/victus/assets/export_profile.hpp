// SPDX-License-Identifier: Apache-2.0
// Per-platform texture profiles for game exports (ADR-0149).
//
// The cooker keeps every texture as a full RGBA8 mip chain plus block-compressed variants for
// desktop (BC) and mobile (ASTC) GPUs, so one cache serves the editor on any machine. A shipped
// game only needs what its target samples: an Android export drops the BC variants, caps the
// resolution for phone screens and keeps the large levels only as ASTC (the runtime decodes them
// on the CPU on the rare device that cannot sample ASTC). This shrinks a pack roughly 5-8x.
#pragma once
#include "victus/assets/cooked.hpp"
#include "victus/core/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace victus::assets {

struct TextureExportProfile {
    std::string target{"all"};
    std::uint32_t max_size{0};          // top levels larger than this are dropped (0 = keep)
    bool drop_desktop_codecs{false};    // BC1/BC3/BC7
    bool drop_mobile_codecs{false};     // ASTC
    // Levels whose larger side exceeds this keep no RGBA8 copy when a compressed variant
    // remains (0 = keep every RGBA8 level). Small levels stay for CPU users (lightmap colour).
    std::uint32_t rgba8_max_size{0};
};

// "all" (unchanged cache), "desktop" (drops ASTC) or "android" (ASTC only above 64 px, 1024 px cap).
[[nodiscard]] Result<TextureExportProfile> texture_export_profile(std::string_view target);

struct TextureProfileStats {
    std::size_t textures{0};
    std::size_t levels_dropped{0};   // top mips removed by max_size
    std::size_t variants_dropped{0};
    std::size_t rgba8_stripped{0};   // levels now stored only compressed
};

// Applies `profile` in place. HDR textures are left untouched. Keeps the invariant that every
// compressed variant has one level per entry of `mips`.
void apply_texture_profile(CookedTexture& texture, const TextureExportProfile& profile, TextureProfileStats* stats = nullptr);
void apply_texture_profile(CookedModel& model, const TextureExportProfile& profile, TextureProfileStats* stats = nullptr);

// Re-encodes a cooked texture or model file for `profile`. Other asset kinds, and the "all"
// profile, return the bytes unchanged.
[[nodiscard]] Result<std::vector<std::uint8_t>> apply_texture_profile(std::span<const std::uint8_t> cooked, const TextureExportProfile& profile,
                                                                     TextureProfileStats* stats = nullptr);

} // namespace victus::assets
