// SPDX-License-Identifier: Apache-2.0
#include "victus/render/scene_renderer.hpp"

#include <algorithm>
#include <format>

namespace victus::render {
namespace detail {
std::string_view pbr_source() noexcept;
}

std::string_view pbr_fragment_source() noexcept { return detail::pbr_source(); }

std::string surface_shader_source(std::string_view surface_code) {
    const std::string_view base = detail::pbr_source();
    constexpr std::string_view marker = "// VICTUS_SURFACE_CODE";
    const auto at = base.find(marker);
    if (at == std::string_view::npos) return std::string(base); // cannot happen with the shipped pbr.frag
    const auto line_end = base.find('\n', at);
    const std::size_t resume = line_end == std::string_view::npos ? base.size() : line_end + 1;
    // Line number (1-based) of the first base line after the marker, for the closing #line.
    const auto next_line = static_cast<std::size_t>(std::count(base.begin(), base.begin() + static_cast<std::ptrdiff_t>(resume), '\n')) + 1;
    std::string out;
    out.reserve(base.size() + surface_code.size() + 128);
    out.append(base.substr(0, at));
    out += "#define VICTUS_SURFACE_SHADER 1\n#line 1\n";
    out.append(surface_code);
    if (!surface_code.empty() && surface_code.back() != '\n') out += '\n';
    out += std::format("#line {}\n", next_line);
    out.append(base.substr(resume));
    return out;
}

} // namespace victus::render
