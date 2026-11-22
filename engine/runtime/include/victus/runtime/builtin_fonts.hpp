// SPDX-License-Identifier: Apache-2.0
// The Inter fonts (SIL OFL 1.1, third_party/fonts/inter) embedded in the runtime library, used
// by the in-game UI and by VICTUS Studio. Each weight comes as a Latin-1 base set plus Latin
// Extended, Cyrillic and Greek sets meant to be merged into it. Noto Sans Bengali and Arabic
// (SIL OFL 1.1, third_party/fonts/noto) are the in-game UI's fallbacks for those scripts.
#pragma once

#include <span>

namespace victus::runtime {

namespace detail {
std::span<const unsigned char> inter_Medium() noexcept;
std::span<const unsigned char> inter_Bold() noexcept;
std::span<const unsigned char> inter_Medium_latin_ext() noexcept;
std::span<const unsigned char> inter_Bold_latin_ext() noexcept;
std::span<const unsigned char> inter_Medium_cyrillic() noexcept;
std::span<const unsigned char> inter_Bold_cyrillic() noexcept;
std::span<const unsigned char> inter_Medium_greek() noexcept;
std::span<const unsigned char> inter_Bold_greek() noexcept;
std::span<const unsigned char> noto_Bengali_Medium() noexcept;
std::span<const unsigned char> noto_Bengali_Bold() noexcept;
std::span<const unsigned char> noto_Arabic_Medium() noexcept;
std::span<const unsigned char> noto_Arabic_Bold() noexcept;
} // namespace detail

namespace builtin_fonts {
inline std::span<const unsigned char> inter_medium() noexcept { return detail::inter_Medium(); }
inline std::span<const unsigned char> inter_bold() noexcept { return detail::inter_Bold(); }
inline std::span<const unsigned char> inter_medium_latin_ext() noexcept { return detail::inter_Medium_latin_ext(); }
inline std::span<const unsigned char> inter_bold_latin_ext() noexcept { return detail::inter_Bold_latin_ext(); }
inline std::span<const unsigned char> inter_medium_cyrillic() noexcept { return detail::inter_Medium_cyrillic(); }
inline std::span<const unsigned char> inter_bold_cyrillic() noexcept { return detail::inter_Bold_cyrillic(); }
inline std::span<const unsigned char> inter_medium_greek() noexcept { return detail::inter_Medium_greek(); }
inline std::span<const unsigned char> inter_bold_greek() noexcept { return detail::inter_Bold_greek(); }
} // namespace builtin_fonts

} // namespace victus::runtime
