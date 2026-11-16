// SPDX-License-Identifier: Apache-2.0
#include "victus/assets/cooked.hpp"

#include "victus/assets/binary.hpp"

#include <format>

namespace victus::assets {

namespace {
constexpr char kMagic[5] = {'V', 'C', 'O', 'O', 'K'};
// Hard limits keep corrupted files from triggering huge allocations.
constexpr std::uint32_t kMaxTextureDim = 16384;
constexpr std::uint64_t kMaxItems = 1u << 20;

void write_header(BinaryWriter& w, const CookedHeader& h) {
    for (const char c : kMagic) w.u8(static_cast<std::uint8_t>(c));
    w.u32(kCookedFormatVersion);
    w.u8(static_cast<std::uint8_t>(h.kind));
    w.u64(h.key);
    w.u32(static_cast<std::uint32_t>(h.dependencies.size()));
    for (const auto& d : h.dependencies) {
        w.str(d.path);
        w.u64(d.hash);
    }
}

bool parse_header(BinaryReader& r, CookedHeader& h) {
    for (const char c : kMagic) {
        std::uint8_t b = 0;
        if (!r.u8(b) || b != static_cast<std::uint8_t>(c)) return false;
    }
    std::uint8_t kind = 0;
    std::uint32_t deps = 0;
    if (!r.u32(h.version) || !r.u8(kind) || !r.u64(h.key) || !r.u32(deps) || deps > 4096) return false;
    if (kind > static_cast<std::uint8_t>(AssetKind::Scene)) return false;
    h.kind = static_cast<AssetKind>(kind);
    h.dependencies.resize(deps);
    for (auto& d : h.dependencies) {
        if (!r.str(d.path, 4096) || !r.u64(d.hash)) return false;
    }
    return true;
}

void write_texture_body(BinaryWriter& w, const CookedTexture& t) {
    w.str(t.name);
    w.u8(t.srgb ? 1 : 0);
    w.u32(static_cast<std::uint32_t>(t.mips.size()));
    for (const auto& m : t.mips) {
        w.u32(m.width);
        w.u32(m.height);
        w.raw(m.pixels);
    }
}

bool read_texture_body(BinaryReader& r, CookedTexture& t) {
    std::uint32_t mips = 0;
    if (!r.str(t.name) || !r.boolean(t.srgb) || !r.u32(mips) || mips == 0 || mips > 16) return false;
    t.mips.resize(mips);
    for (auto& m : t.mips) {
        if (!r.u32(m.width) || !r.u32(m.height)) return false;
        if (m.width == 0 || m.height == 0 || m.width > kMaxTextureDim || m.height > kMaxTextureDim) return false;
        if (!r.raw(m.pixels, std::uint64_t{m.width} * m.height * 4)) return false;
        if (m.pixels.size() != std::size_t{m.width} * m.height * 4) return false;
    }
    return true;
}

void write_color(BinaryWriter& w, math::Color c) {
    w.f32(c.r);
    w.f32(c.g);
    w.f32(c.b);
    w.f32(c.a);
}
bool read_color(BinaryReader& r, math::Color& c) { return r.f32(c.r) && r.f32(c.g) && r.f32(c.b) && r.f32(c.a); }

Error corrupt(std::string_view what) {
    return make_error(ErrorCode::ParseError, std::format("cooked asset is corrupt or truncated ({})", what));
}
} // namespace

const char* to_string(AssetKind kind) noexcept {
    switch (kind) {
    case AssetKind::Texture: return "texture";
    case AssetKind::Model: return "model";
    case AssetKind::Scene: return "scene";
    case AssetKind::Script: return "script";
    default: return "unknown";
    }
}

std::size_t CookedModel::triangle_count() const noexcept {
    std::size_t n = 0;
    for (const auto& m : meshes) n += m.data.triangle_count();
    return n;
}

std::vector<std::uint8_t> serialize(const CookedHeader& header, const CookedTexture& texture) {
    BinaryWriter w;
    CookedHeader h = header;
    h.kind = AssetKind::Texture;
    write_header(w, h);
    write_texture_body(w, texture);
    return w.take();
}

std::vector<std::uint8_t> serialize(const CookedHeader& header, const CookedModel& model) {
    BinaryWriter w;
    CookedHeader h = header;
    h.kind = AssetKind::Model;
    write_header(w, h);
    w.u32(static_cast<std::uint32_t>(model.textures.size()));
    for (const auto& t : model.textures) write_texture_body(w, t);
    w.u32(static_cast<std::uint32_t>(model.materials.size()));
    for (const auto& m : model.materials) {
        w.str(m.name);
        write_color(w, m.base_color);
        write_color(w, m.emissive);
        w.f32(m.emissive_strength);
        w.f32(m.metallic);
        w.f32(m.roughness);
        w.f32(m.normal_scale);
        w.f32(m.alpha_cutoff);
        w.u8(m.double_sided ? 1 : 0);
        w.u8(m.unlit ? 1 : 0);
        w.i32(m.base_color_texture);
        w.i32(m.metallic_roughness_texture);
        w.i32(m.normal_texture);
        w.i32(m.emissive_texture);
    }
    w.u32(static_cast<std::uint32_t>(model.meshes.size()));
    for (const auto& m : model.meshes) {
        w.str(m.name);
        w.u32(m.material);
        w.array(m.data.vertices);
        w.array(m.data.indices);
    }
    w.u32(static_cast<std::uint32_t>(model.nodes.size()));
    for (const auto& n : model.nodes) {
        w.str(n.name);
        w.i32(n.parent);
        w.pod(n.translation);
        w.pod(n.rotation);
        w.pod(n.scale);
        w.array(n.meshes);
    }
    return w.take();
}

Result<CookedHeader> read_header(std::span<const std::uint8_t> bytes) {
    BinaryReader r(bytes);
    CookedHeader h;
    if (!parse_header(r, h)) return corrupt("header");
    if (h.version != kCookedFormatVersion)
        return make_error(ErrorCode::VersionMismatch, std::format("cooked format version {} (expected {})", h.version, kCookedFormatVersion));
    return h;
}

Result<CookedTexture> deserialize_texture(std::span<const std::uint8_t> bytes) {
    BinaryReader r(bytes);
    CookedHeader h;
    if (!parse_header(r, h)) return corrupt("header");
    if (h.version != kCookedFormatVersion) return make_error(ErrorCode::VersionMismatch, "cooked texture version");
    if (h.kind != AssetKind::Texture) return make_error(ErrorCode::InvalidArgument, "cooked asset is not a texture");
    CookedTexture t;
    if (!read_texture_body(r, t) || !r.at_end()) return corrupt("texture");
    return t;
}

Result<CookedModel> deserialize_model(std::span<const std::uint8_t> bytes) {
    BinaryReader r(bytes);
    CookedHeader h;
    if (!parse_header(r, h)) return corrupt("header");
    if (h.version != kCookedFormatVersion) return make_error(ErrorCode::VersionMismatch, "cooked model version");
    if (h.kind != AssetKind::Model) return make_error(ErrorCode::InvalidArgument, "cooked asset is not a model");
    CookedModel m;
    std::uint32_t n = 0;
    if (!r.u32(n) || n > kMaxItems) return corrupt("texture count");
    m.textures.resize(n);
    for (auto& t : m.textures)
        if (!read_texture_body(r, t)) return corrupt("model texture");
    if (!r.u32(n) || n > kMaxItems) return corrupt("material count");
    m.materials.resize(n);
    for (auto& mat : m.materials) {
        if (!r.str(mat.name) || !read_color(r, mat.base_color) || !read_color(r, mat.emissive) ||
            !r.f32(mat.emissive_strength) || !r.f32(mat.metallic) || !r.f32(mat.roughness) || !r.f32(mat.normal_scale) ||
            !r.f32(mat.alpha_cutoff) || !r.boolean(mat.double_sided) || !r.boolean(mat.unlit) ||
            !r.i32(mat.base_color_texture) || !r.i32(mat.metallic_roughness_texture) || !r.i32(mat.normal_texture) ||
            !r.i32(mat.emissive_texture))
            return corrupt("material");
        for (const std::int32_t ti : {mat.base_color_texture, mat.metallic_roughness_texture, mat.normal_texture, mat.emissive_texture})
            if (ti < -1 || ti >= static_cast<std::int32_t>(m.textures.size())) return corrupt("material texture index");
    }
    if (!r.u32(n) || n > kMaxItems) return corrupt("mesh count");
    m.meshes.resize(n);
    for (auto& mesh : m.meshes) {
        if (!r.str(mesh.name) || !r.u32(mesh.material) || !r.array(mesh.data.vertices) || !r.array(mesh.data.indices))
            return corrupt("mesh");
        if (mesh.material >= m.materials.size()) return corrupt("mesh material index");
        for (const std::uint32_t i : mesh.data.indices)
            if (i >= mesh.data.vertices.size()) return corrupt("mesh index out of range");
    }
    if (!r.u32(n) || n > kMaxItems) return corrupt("node count");
    m.nodes.resize(n);
    for (std::size_t i = 0; i < m.nodes.size(); ++i) {
        auto& node = m.nodes[i];
        if (!r.str(node.name) || !r.i32(node.parent) || !r.pod(node.translation) || !r.pod(node.rotation) ||
            !r.pod(node.scale) || !r.array(node.meshes))
            return corrupt("node");
        if (node.parent < -1 || node.parent >= static_cast<std::int32_t>(i)) return corrupt("node parent order");
        for (const std::uint32_t mi : node.meshes)
            if (mi >= m.meshes.size()) return corrupt("node mesh index");
    }
    if (!r.at_end()) return corrupt("trailing bytes");
    return m;
}

} // namespace victus::assets
