// SPDX-License-Identifier: Apache-2.0
#include "victus/render/scene_renderer.hpp"

#include <algorithm>
#include <cctype>
#include <utility>
#include <charconv>
#include <format>
#include <regex>

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


// Parses "1", "-0.5", "vec3(1, 0.5, 0)" (or "vec3(1)") into up to `n` components.
bool parse_value(std::string_view text, std::size_t n, math::Vec4& out) {
    auto trim = [](std::string_view v) {
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front())) != 0) v.remove_prefix(1);
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back())) != 0) v.remove_suffix(1);
        return v;
    };
    auto number = [&](std::string_view v, float& f) {
        v = trim(v);
        if (!v.empty() && v.front() == '+') v.remove_prefix(1);
        if (!v.empty() && (v.back() == 'f' || v.back() == 'F')) v.remove_suffix(1);
        const auto r = std::from_chars(v.data(), v.data() + v.size(), f);
        return !v.empty() && r.ec == std::errc{} && r.ptr == v.data() + v.size();
    };
    text = trim(text);
    float c[4] = {0, 0, 0, 0};
    std::size_t count = 0;
    if (const auto open = text.find('('); open != std::string_view::npos) {
        const std::string_view ctor = trim(text.substr(0, open));
        if (text.back() != ')' || ctor != (n == 1 ? std::string_view("float") : std::string_view(std::format("vec{}", n))))
            return false;
        std::string_view args = text.substr(open + 1, text.size() - open - 2);
        while (true) {
            const auto comma = args.find(',');
            if (count == 4 || !number(args.substr(0, comma), c[count])) return false;
            ++count;
            if (comma == std::string_view::npos) break;
            args.remove_prefix(comma + 1);
        }
        if (count != 1 && count != n) return false;
    } else {
        if (!number(text, c[0])) return false;
        count = 1;
    }
    if (count == 1)
        for (std::size_t i = 1; i < n; ++i) c[i] = c[0];
    out = {c[0], c[1], c[2], c[3]};
    return true;
}

// Line [begin, end) offsets of every line of `text`.
std::vector<std::pair<std::size_t, std::size_t>> lines_of(const std::string& text) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    std::size_t b = 0;
    while (b <= text.size()) {
        const auto e = text.find('\n', b);
        out.emplace_back(b, e == std::string::npos ? text.size() : e);
        if (e == std::string::npos) break;
        b = e + 1;
    }
    return out;
}

const std::regex& declaration_regex() {
    static const std::regex re(R"(^\s*uniform\s+(float|vec2|vec3|vec4|sampler2D)\s+([A-Za-z_]\w*)\s*)"
                               R"((?::\s*([A-Za-z_]\w*)\s*(?:\(([^)]*)\))?\s*)?(?:=\s*([^;]*?))?\s*;\s*$)");
    return re;
}

enum class Stage { Fragment, Vertex };

// `code` (comments kept) with each declaration line replaced for `stage`; `count` gets the
// number of textures. Lines that do not parse are kept (the compiler reports them).
std::string rewrite_declarations(std::string_view code, Stage stage, std::size_t& texture_count) {
    const SurfaceDeclarations d = parse_surface_declarations(code);
    texture_count = d.textures.size();
    std::string out(code);
    if (d.params.empty() && d.textures.empty()) return out;
    const auto lines = lines_of(out);
    std::string result;
    result.reserve(out.size() + 128);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto line = static_cast<std::uint32_t>(i + 1);
        std::string text = out.substr(lines[i].first, lines[i].second - lines[i].first);
        for (const SurfaceParamDecl& p : d.params)
            if (p.line == line) {
                static constexpr const char* swizzle[] = {"", ".x", ".xy", ".xyz", ""};
                text = std::format("#define {} {}[{}]{}", p.name, stage == Stage::Fragment ? "material.custom" : "vx_material.params",
                                   p.slot, swizzle[p.components]);
            }
        for (const SurfaceTextureDecl& t : d.textures)
            if (t.line == line)
                text = std::format("layout(set = {}, binding = {}) uniform sampler2D {};", stage == Stage::Fragment ? 2 : 0,
                                   (stage == Stage::Fragment ? 12 : 0) + t.slot, t.name);
        result += text;
        if (i + 1 < lines.size()) result += '\n';
    }
    return result;
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
    std::size_t textures = 0;
    const std::string code = rewrite_declarations(surface_code_without_function(surface_code, "vertex"), Stage::Fragment, textures);
    const auto version_end = base.find('\n') + 1; // "#version 450"
    std::string out;
    out.reserve(base.size() + surface_code.size() + 192);
    out.append(base.substr(0, version_end));
    // Custom textures take bindings 12.. of set 2; the storage buffers move after them.
    out += std::format("#define VX_CUSTOM_TEXTURES {}\n#line 2\n", textures);
    out.append(base.substr(version_end, at - version_end));
    out += "#define VICTUS_SURFACE_SHADER 1\n#define VICTUS_FRAGMENT_STAGE 1\n#line 1\n";
    out.append(code);
    if (!code.empty() && code.back() != '\n') out += '\n';
    out += std::format("#line {}\n", next_line);
    out.append(base.substr(resume));
    return out;
}

SurfaceDeclarations parse_surface_declarations(std::string_view code) {
    SurfaceDeclarations d;
    const std::string text = blank_comments(code);
    const auto lines = lines_of(text);
    auto fail = [&](std::uint32_t line, std::string message) {
        if (d.error.empty()) {
            d.error = std::move(message);
            d.error_line = line;
        }
    };
    auto taken = [&](const std::string& name) {
        return std::any_of(d.params.begin(), d.params.end(), [&](const SurfaceParamDecl& p) { return p.name == name; }) ||
               std::any_of(d.textures.begin(), d.textures.end(), [&](const SurfaceTextureDecl& t) { return t.name == name; });
    };
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string line = text.substr(lines[i].first, lines[i].second - lines[i].first);
        std::smatch m;
        if (!std::regex_match(line, m, declaration_regex())) continue;
        const auto no = static_cast<std::uint32_t>(i + 1);
        const std::string type = m[1].str(), name = m[2].str(), hint = m[3].str(), hint_args = m[4].str(), value = m[5].str();
        if (name.starts_with("gl_") || name.starts_with("u_") || name.starts_with("vx_") || name == "material" || name.starts_with("VICTUS")) {
            fail(no, std::format("'{}' is a reserved name", name));
            continue;
        }
        if (taken(name)) {
            fail(no, std::format("'{}' is declared twice", name));
            continue;
        }
        if (type == "sampler2D") {
            SurfaceTextureDecl t;
            t.name = name;
            t.line = no;
            if (!value.empty()) fail(no, std::format("texture '{}' cannot have a value (use hint_default_black or hint_normal)", name));
            if (hint == "hint_default_black") t.fallback = SurfaceTextureDefault::Black;
            else if (hint == "hint_normal") t.fallback = SurfaceTextureDefault::Normal;
            else if (!hint.empty() && hint != "hint_default_white" && hint != "source_color")
                fail(no, std::format("unknown texture hint '{}' (hint_default_white, hint_default_black, hint_normal)", hint));
            if (d.textures.size() == kMaxSurfaceTextures) {
                fail(no, std::format("more than {} textures", kMaxSurfaceTextures));
                continue;
            }
            t.slot = static_cast<std::uint8_t>(d.textures.size());
            d.textures.push_back(std::move(t));
            continue;
        }
        SurfaceParamDecl p;
        p.name = name;
        p.line = no;
        p.components = static_cast<std::uint8_t>(type == "float" ? 1 : type[3] - '0');
        if (!value.empty() && !parse_value(value, p.components, p.value))
            fail(no, std::format("cannot read the value of '{}' (write a number or {}(...))", name, type));
        if (hint == "source_color") {
            if (p.components < 3) fail(no, "source_color needs a vec3 or vec4");
            p.color = true;
        } else if (hint == "hint_range") {
            math::Vec4 r;
            const auto commas = std::count(hint_args.begin(), hint_args.end(), ',');
            if (commas < 1 || commas > 2 || !parse_value(std::format("vec{}({})", commas + 1, hint_args), static_cast<std::size_t>(commas + 1), r))
                fail(no, "hint_range takes (min, max) or (min, max, step)");
            p.has_range = true;
            p.min = r.x;
            p.max = r.y;
            p.step = commas == 2 ? r.z : 0.0f;
        } else if (!hint.empty()) {
            fail(no, std::format("unknown parameter hint '{}' (hint_range, source_color)", hint));
        }
        if (d.params.size() == kMaxSurfaceParams) {
            fail(no, std::format("more than {} parameters", kMaxSurfaceParams));
            continue;
        }
        p.slot = static_cast<std::uint8_t>(d.params.size());
        d.params.push_back(std::move(p));
    }
    return d;
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
    std::size_t textures = 0;
    const std::string code = rewrite_declarations(surface_code_without_function(surface_code, "surface"), Stage::Vertex, textures);
    defines += std::format("#define VX_CUSTOM_TEXTURES {}\n", textures);
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
