// SPDX-License-Identifier: Apache-2.0
#include "victus/assets/cooked.hpp"

#include <cmath>

#include <cstring>

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
    w.u8(t.hdr ? 1 : 0);
    if (t.hdr) {
        w.u32(t.hdr_width);
        w.u32(t.hdr_height);
        w.raw({reinterpret_cast<const std::uint8_t*>(t.hdr_pixels.data()), t.hdr_pixels.size() * 2});
        for (const auto& c : t.sh) {
            w.f32(c.x);
            w.f32(c.y);
            w.f32(c.z);
        }
    }
    w.u32(static_cast<std::uint32_t>(t.compressed.size()));
    for (const auto& v : t.compressed) {
        w.u8(static_cast<std::uint8_t>(v.codec));
        for (const auto& level : v.levels) w.raw(level);
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
    if (!r.boolean(t.hdr)) return false;
    if (t.hdr) {
        if (!r.u32(t.hdr_width) || !r.u32(t.hdr_height)) return false;
        if (t.hdr_width == 0 || t.hdr_height == 0 || t.hdr_width > kMaxTextureDim || t.hdr_height > kMaxTextureDim) return false;
        std::vector<std::uint8_t> raw;
        const std::uint64_t bytes = std::uint64_t{t.hdr_width} * t.hdr_height * 8;
        if (!r.raw(raw, bytes) || raw.size() != bytes) return false;
        t.hdr_pixels.resize(raw.size() / 2);
        std::memcpy(t.hdr_pixels.data(), raw.data(), raw.size());
        for (auto& c : t.sh)
            if (!r.f32(c.x) || !r.f32(c.y) || !r.f32(c.z) || !std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z))
                return false;
    }
    std::uint32_t variants = 0;
    if (!r.u32(variants) || variants > 8) return false;
    t.compressed.resize(variants);
    for (auto& v : t.compressed) {
        std::uint8_t codec = 0;
        if (!r.u8(codec) || !parse_texture_codec(to_string(static_cast<TextureCodec>(codec)))) return false;
        v.codec = static_cast<TextureCodec>(codec);
        v.levels.resize(t.mips.size());
        for (std::size_t m = 0; m < t.mips.size(); ++m) {
            const std::uint64_t bytes = rhi::mip_size_bytes(codec_format(v.codec, t.srgb), t.mips[m].width, t.mips[m].height);
            if (!r.raw(v.levels[m], bytes) || v.levels[m].size() != bytes) return false;
        }
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
    case AssetKind::Audio: return "audio";
    case AssetKind::Prefab: return "prefab";
    case AssetKind::Material: return "material";
    case AssetKind::Shader: return "shader";
    case AssetKind::SpriteSheet: return "spritesheet";
    case AssetKind::Tileset: return "tileset";
    case AssetKind::Terrain: return "terrain";
    case AssetKind::BehaviorTree: return "behavior_tree";
    case AssetKind::Sequence: return "sequence";
    case AssetKind::StringTable: return "string_table";
    case AssetKind::AnimController: return "anim_controller";
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
        w.array(m.data.skin);
        w.u32(static_cast<std::uint32_t>(m.data.lods.size()));
        for (const auto& lod : m.data.lods) {
            w.f32(lod.error);
            w.array(lod.indices);
        }
    }
    w.u32(static_cast<std::uint32_t>(model.nodes.size()));
    for (const auto& n : model.nodes) {
        w.str(n.name);
        w.i32(n.parent);
        w.pod(n.translation);
        w.pod(n.rotation);
        w.pod(n.scale);
        w.array(n.meshes);
        w.i32(n.skin);
    }
    w.u32(static_cast<std::uint32_t>(model.skins.size()));
    for (const auto& skin : model.skins) {
        w.str(skin.name);
        w.array(skin.joints);
        w.array(skin.inverse_bind);
    }
    w.u32(static_cast<std::uint32_t>(model.animations.size()));
    for (const auto& clip : model.animations) {
        w.str(clip.name);
        w.f32(clip.duration);
        w.u32(static_cast<std::uint32_t>(clip.channels.size()));
        for (const auto& ch : clip.channels) {
            w.u32(ch.node);
            w.u8(static_cast<std::uint8_t>(ch.path));
            w.u8(static_cast<std::uint8_t>(ch.interpolation));
            w.array(ch.times);
            w.array(ch.values);
        }
    }
    return w.take();
}

Status validate_model(const CookedModel& model) {
    const auto bad = [](std::string what) { return make_error(ErrorCode::InvalidArgument, std::move(what)); };
    for (std::size_t si = 0; si < model.skins.size(); ++si) {
        const ModelSkin& skin = model.skins[si];
        if (skin.joints.empty()) return bad(std::format("skin {} has no joints", si));
        if (skin.joints.size() > 65535) return bad(std::format("skin {} has too many joints", si));
        if (skin.inverse_bind.size() != skin.joints.size()) return bad(std::format("skin {} inverse bind count mismatch", si));
        for (const std::uint32_t j : skin.joints)
            if (j >= model.nodes.size()) return bad(std::format("skin {} joint node out of range", si));
    }
    for (const ModelMesh& mesh : model.meshes)
        if (!mesh.data.skin.empty() && mesh.data.skin.size() != mesh.data.vertices.size())
            return bad(std::format("mesh '{}' skin stream size mismatch", mesh.name));
    for (const ModelNode& node : model.nodes) {
        if (node.skin < -1 || node.skin >= static_cast<std::int32_t>(model.skins.size())) return bad("node skin index out of range");
        if (node.skin < 0) continue;
        const std::size_t joints = model.skins[static_cast<std::size_t>(node.skin)].joints.size();
        for (const std::uint32_t mi : node.meshes) {
            for (const render::SkinVertex& v : model.meshes[mi].data.skin)
                for (const std::uint16_t j : v.joints)
                    if (j >= joints) return bad(std::format("mesh '{}' joint index exceeds skin '{}'", model.meshes[mi].name, node.name));
        }
    }
    for (const auto& clip : model.animations)
        if (auto st = animation::validate(clip, model.nodes.size()); !st) return st;
    return {};
}

animation::AnimationSet make_animation_set(const CookedModel& model) {
    animation::AnimationSet set;
    set.nodes.reserve(model.nodes.size());
    for (const ModelNode& n : model.nodes)
        set.nodes.push_back({.name = n.name, .parent = n.parent, .rest = {n.translation, n.rotation, n.scale}});
    set.clips = model.animations;
    return set;
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
        if (!r.str(mesh.name) || !r.u32(mesh.material) || !r.array(mesh.data.vertices) || !r.array(mesh.data.indices) ||
            !r.array(mesh.data.skin))
            return corrupt("mesh");
        if (mesh.material >= m.materials.size()) return corrupt("mesh material index");
        for (const std::uint32_t i : mesh.data.indices)
            if (i >= mesh.data.vertices.size()) return corrupt("mesh index out of range");
        std::uint32_t lods = 0;
        if (!r.u32(lods) || lods >= render::kMaxMeshLods) return corrupt("mesh lod count");
        mesh.data.lods.resize(lods);
        for (auto& lod : mesh.data.lods) {
            if (!r.f32(lod.error) || !std::isfinite(lod.error) || lod.error < 0 || !r.array(lod.indices) ||
                lod.indices.size() % 3 != 0)
                return corrupt("mesh lod");
            for (const std::uint32_t i : lod.indices)
                if (i >= mesh.data.vertices.size()) return corrupt("mesh lod index out of range");
        }
    }
    if (!r.u32(n) || n > kMaxItems) return corrupt("node count");
    m.nodes.resize(n);
    for (std::size_t i = 0; i < m.nodes.size(); ++i) {
        auto& node = m.nodes[i];
        if (!r.str(node.name) || !r.i32(node.parent) || !r.pod(node.translation) || !r.pod(node.rotation) ||
            !r.pod(node.scale) || !r.array(node.meshes) || !r.i32(node.skin))
            return corrupt("node");
        if (node.parent < -1 || node.parent >= static_cast<std::int32_t>(i)) return corrupt("node parent order");
        for (const std::uint32_t mi : node.meshes)
            if (mi >= m.meshes.size()) return corrupt("node mesh index");
    }
    if (!r.u32(n) || n > kMaxItems) return corrupt("skin count");
    m.skins.resize(n);
    for (auto& skin : m.skins)
        if (!r.str(skin.name) || !r.array(skin.joints, kMaxItems) || !r.array(skin.inverse_bind, kMaxItems)) return corrupt("skin");
    if (!r.u32(n) || n > kMaxItems) return corrupt("animation count");
    m.animations.resize(n);
    for (auto& clip : m.animations) {
        std::uint32_t channels = 0;
        if (!r.str(clip.name) || !r.f32(clip.duration) || !r.u32(channels) || channels > kMaxItems) return corrupt("animation");
        clip.channels.resize(channels);
        for (auto& ch : clip.channels) {
            std::uint8_t path = 0, interp = 0;
            if (!r.u32(ch.node) || !r.u8(path) || !r.u8(interp) || path > 2 || interp > 2 || !r.array(ch.times) ||
                !r.array(ch.values))
                return corrupt("animation channel");
            ch.path = static_cast<animation::Path>(path);
            ch.interpolation = static_cast<animation::Interpolation>(interp);
        }
    }
    if (!r.at_end()) return corrupt("trailing bytes");
    if (auto st = validate_model(m); !st) return corrupt(st.error().message);
    return m;
}

render::ImageF CookedTexture::hdr_image() const {
    render::ImageF img;
    if (!hdr) return img;
    img.width = hdr_width;
    img.height = hdr_height;
    img.pixels.resize(hdr_pixels.size());
    for (std::size_t i = 0; i < hdr_pixels.size(); ++i) img.pixels[i] = render::half_to_float(hdr_pixels[i]);
    return img;
}

} // namespace victus::assets
