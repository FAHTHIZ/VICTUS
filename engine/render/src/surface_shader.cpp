// SPDX-License-Identifier: Apache-2.0
#include "victus/render/scene_renderer.hpp"

#include <algorithm>
#include <cctype>
#include <utility>
#include <format>

namespace victus::render {
namespace detail {
std::string_view pbr_source() noexcept;
std::string_view surface_vertex_source_text() noexcept;
} // namespace detail

namespace {

// `code` with comments replaced by spaces (newlines kept), so searches skip commented-out code.
std::string blank_comments(std::string_view code) {
    std::string out(code);
    for (std::size_t i = 0; i + 1 < out.size();) {
        if (out[i] == '/' && out[i + 1] == '/') {
            while (i < out.size() && out[i] != '\n') out[i++] = ' ';
        } else if (out[i] == '/' && out[i + 1] == '*') {
            out[i] = out[i + 1] = ' ';
            i += 2;
            while (i < out.size() && !(out[i] == '*' && i + 1 < out.size() && out[i + 1] == '/')) {
                if (out[i] != '\n') out[i] = ' ';
                ++i;
            }
            if (i < out.size()) out[i++] = ' ';
            if (i < out.size()) out[i++] = ' ';
        } else {
            ++i;
        }
    }
    return out;
}

bool ident_char(char c) noexcept { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

// [begin, end) of the definition `void name(...) { ... }` in comment-free `text`; begin == end
// when there is none (prototypes without a body are skipped).
std::pair<std::size_t, std::size_t> find_function(const std::string& text, std::string_view name) {
    for (std::size_t at = text.find(name); at != std::string::npos; at = text.find(name, at + 1)) {
        if (at > 0 && ident_char(text[at - 1])) continue;
        std::size_t k = at + name.size();
        if (k < text.size() && ident_char(text[k])) continue;
        while (k < text.size() && std::isspace(static_cast<unsigned char>(text[k])) != 0) ++k;
        if (k >= text.size() || text[k] != '(') continue;
        // Return type "void" right before the name.
        std::size_t r = at;
        while (r > 0 && std::isspace(static_cast<unsigned char>(text[r - 1])) != 0) --r;
        if (r < 4 || text.compare(r - 4, 4, "void") != 0 || (r > 4 && ident_char(text[r - 5]))) continue;
        const std::size_t close = text.find(')', k);
        if (close == std::string::npos) continue;
        std::size_t b = close + 1;
        while (b < text.size() && std::isspace(static_cast<unsigned char>(text[b])) != 0) ++b;
        if (b >= text.size() || text[b] != '{') continue;
        int depth = 0;
        for (std::size_t e = b; e < text.size(); ++e) {
            if (text[e] == '{') ++depth;
            else if (text[e] == '}' && --depth == 0) return {r - 4, e + 1};
        }
        return {r - 4, text.size()};
    }
    return {0, 0};
}

} // namespace

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
    out += "#define VICTUS_SURFACE_SHADER 1\n#define VICTUS_FRAGMENT_STAGE 1\n#line 1\n";
    const std::string code = surface_code_without_function(surface_code, "vertex");
    out.append(code);
    if (!code.empty() && code.back() != '\n') out += '\n';
    out += std::format("#line {}\n", next_line);
    out.append(base.substr(resume));
    return out;
}

bool surface_code_has_function(std::string_view code, std::string_view name) {
    const auto [b, e] = find_function(blank_comments(code), name);
    return e > b;
}

std::string surface_code_without_function(std::string_view code, std::string_view name) {
    std::string out(code);
    const auto [b, e] = find_function(blank_comments(code), name);
    for (std::size_t i = b; i < e; ++i)
        if (out[i] != '\n') out[i] = ' ';
    return out;
}

std::string surface_vertex_source(std::string_view surface_code, SurfaceVertexVariant variant) {
    const std::string_view base = detail::surface_vertex_source_text();
    std::string defines;
    if (variant == SurfaceVertexVariant::Skinned || variant == SurfaceVertexVariant::ShadowSkinned) defines += "#define VX_SKINNED 1\n";
    if (variant == SurfaceVertexVariant::Lightmapped) defines += "#define VX_LIGHTMAP 1\n";
    if (variant == SurfaceVertexVariant::Shadow || variant == SurfaceVertexVariant::ShadowSkinned) defines += "#define VX_SHADOW 1\n";
    const auto version_end = base.find('\n') + 1; // "#version 450"
    constexpr std::string_view marker = "// VICTUS_VERTEX_CODE";
    const auto at = base.find(marker);
    const auto line_end = base.find('\n', at);
    const std::size_t resume = line_end == std::string_view::npos ? base.size() : line_end + 1;
    const auto lines_before = [&](std::size_t pos) {
        return static_cast<std::size_t>(std::count(base.begin(), base.begin() + static_cast<std::ptrdiff_t>(pos), '\n'));
    };
    const std::string code = surface_code_without_function(surface_code, "surface");
    std::string out;
    out.reserve(base.size() + code.size() + 256);
    out.append(base.substr(0, version_end));
    out += defines;
    out += std::format("#line {}\n", lines_before(version_end) + 1);
    out.append(base.substr(version_end, at - version_end));
    out += "#line 1\n";
    out.append(code);
    if (!code.empty() && code.back() != '\n') out += '\n';
    out += std::format("#line {}\n", lines_before(resume) + 1);
    out.append(base.substr(resume));
    return out;
}

} // namespace victus::render
