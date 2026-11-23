// SPDX-License-Identifier: Apache-2.0
#pragma once
// IES photometric profiles (ADR-0235): IESNA LM-63 (1986, 1991, 1995, 2002) files as published
// by luminaire manufacturers. A profile is baked into a latitude-longitude light cookie
// (ADR-0201) for point lights, so a scene's downlights, wall washers and street lamps throw the
// measured beam shape of the real fitting.
#include "victus/core/result.hpp"
#include "victus/render/image.hpp"

#include <string_view>
#include <vector>

namespace victus::assets {

struct IesProfile {
    // Candela grid: candela[h * vertical.size() + v] at horizontal angle horizontal[h] and
    // vertical angle vertical[v] (degrees; vertical 0 = straight down, 180 = straight up;
    // horizontal 0..360 around the vertical axis). Already scaled by the file's multiplier.
    std::vector<float> vertical;
    std::vector<float> horizontal;
    std::vector<float> candela;
    float lumens_per_lamp{0.0f}; // -1 in absolute photometry files
    int lamps{1};
    float peak_candela{0.0f};
    // Candela towards (vertical, horizontal) degrees, with the file's symmetry applied and 0
    // outside the measured vertical range. Bilinear between the measured angles.
    [[nodiscard]] float sample(float vertical_deg, float horizontal_deg) const noexcept;
};

// Parses an LM-63 file (Type C photometry; TILT=NONE or TILT=INCLUDE, whose tilt table is
// skipped). Types A and B and TILT=<file> are rejected.
[[nodiscard]] Result<IesProfile> parse_ies(std::string_view text);

// The profile as a `size` x `size` latitude-longitude cookie (ADR-0201 mapping: top row up, centre
// column along the light's forward, columns turning towards its right; IES horizontal angle 0 is
// forward and 90 is right), grey, normalised to the peak candela and sRGB-encoded.
[[nodiscard]] render::Image ies_cookie_image(const IesProfile& profile, std::uint32_t size = 256);

} // namespace victus::assets
