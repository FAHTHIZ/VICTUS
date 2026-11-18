// SPDX-License-Identifier: Apache-2.0
//
// VICTUS Studio design tokens: the single source of colour, spacing, radius and
// type-scale values for every editor and in-engine debug surface. UI code must
// use these (or `tone()`) instead of literal ImVec4 colours so the whole Studio
// stays visually consistent and contrast can be verified in one place.
#pragma once

#include <imgui.h>

#include <cmath>

namespace victus::ui::tokens {

// Surfaces, darkest to lightest.
inline constexpr ImVec4 kSurfaceBase{0.075f, 0.080f, 0.092f, 1.0f};   // menu bar, title bars, tab strip
inline constexpr ImVec4 kSurfacePanel{0.110f, 0.116f, 0.132f, 1.0f};  // panel bodies
inline constexpr ImVec4 kSurfaceRaised{0.150f, 0.157f, 0.178f, 1.0f}; // inputs, buttons, cards
inline constexpr ImVec4 kSurfaceHover{0.195f, 0.204f, 0.232f, 1.0f};
inline constexpr ImVec4 kSurfaceActive{0.240f, 0.250f, 0.285f, 1.0f};
inline constexpr ImVec4 kSurfacePopup{0.125f, 0.131f, 0.150f, 0.98f};
inline constexpr ImVec4 kBorder{0.205f, 0.215f, 0.245f, 1.0f};

// Text.
inline constexpr ImVec4 kText{0.925f, 0.932f, 0.950f, 1.0f};
inline constexpr ImVec4 kTextMuted{0.640f, 0.662f, 0.712f, 1.0f};
inline constexpr ImVec4 kTextDisabled{0.450f, 0.470f, 0.520f, 1.0f};

// Brand accent (VICTUS orange) and semantic tones.
inline constexpr ImVec4 kAccent{0.960f, 0.480f, 0.180f, 1.0f};
inline constexpr ImVec4 kAccentHover{1.000f, 0.580f, 0.300f, 1.0f};
inline constexpr ImVec4 kAccentSoft{0.960f, 0.480f, 0.180f, 0.45f};
inline constexpr ImVec4 kAccentSubtle{0.960f, 0.480f, 0.180f, 0.22f};
inline constexpr ImVec4 kSuccess{0.380f, 0.820f, 0.490f, 1.0f};
inline constexpr ImVec4 kWarning{0.980f, 0.760f, 0.310f, 1.0f};
inline constexpr ImVec4 kDanger{0.980f, 0.480f, 0.450f, 1.0f};
inline constexpr ImVec4 kInfo{0.450f, 0.700f, 0.990f, 1.0f};
inline constexpr ImVec4 kInfoSoft{0.450f, 0.700f, 0.990f, 0.40f};

// Spacing scale in pixels at 100 % UI scale.
inline constexpr float kSpace1 = 2.0f;
inline constexpr float kSpace2 = 4.0f;
inline constexpr float kSpace3 = 8.0f;
inline constexpr float kSpace4 = 12.0f;
inline constexpr float kSpace5 = 16.0f;
inline constexpr float kSpace6 = 24.0f;
inline constexpr float kSpace7 = 32.0f;

// Corner radii.
inline constexpr float kRadiusSmall = 3.0f;
inline constexpr float kRadius = 5.0f;
inline constexpr float kRadiusLarge = 8.0f;

// Type scale as multipliers of the base font size.
inline constexpr float kTypeCaption = 0.87f;
inline constexpr float kTypeBody = 1.0f;
inline constexpr float kTypeHeading = 1.15f;
inline constexpr float kTypeTitle = 1.45f;
inline constexpr float kTypeDisplay = 2.1f;

// Base font size (px) used by the Studio at 100 % scale.
inline constexpr float kFontBase = 15.0f;

} // namespace victus::ui::tokens

namespace victus::ui {

enum class Tone { Neutral, Muted, Disabled, Accent, Success, Warning, Danger, Info };

[[nodiscard]] constexpr ImVec4 tone(Tone t) noexcept {
    switch (t) {
    case Tone::Muted:
        return tokens::kTextMuted;
    case Tone::Disabled:
        return tokens::kTextDisabled;
    case Tone::Accent:
        return tokens::kAccent;
    case Tone::Success:
        return tokens::kSuccess;
    case Tone::Warning:
        return tokens::kWarning;
    case Tone::Danger:
        return tokens::kDanger;
    case Tone::Info:
        return tokens::kInfo;
    case Tone::Neutral:
        break;
    }
    return tokens::kText;
}

[[nodiscard]] constexpr ImVec4 with_alpha(ImVec4 c, float a) noexcept { return ImVec4(c.x, c.y, c.z, a); }

/// WCAG 2.x relative luminance of an sRGB colour (alpha ignored).
[[nodiscard]] inline float relative_luminance(const ImVec4& c) noexcept {
    const auto lin = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    return 0.2126f * lin(c.x) + 0.7152f * lin(c.y) + 0.0722f * lin(c.z);
}

/// WCAG contrast ratio between two opaque colours, in [1, 21].
[[nodiscard]] inline float contrast_ratio(const ImVec4& a, const ImVec4& b) noexcept {
    const float la = relative_luminance(a);
    const float lb = relative_luminance(b);
    const float hi = la > lb ? la : lb;
    const float lo = la > lb ? lb : la;
    return (hi + 0.05f) / (lo + 0.05f);
}

} // namespace victus::ui
