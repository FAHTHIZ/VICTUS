// SPDX-License-Identifier: Apache-2.0
#include "victus/assets/importers.hpp"
#include "victus/assets/skeleton.hpp"
#include "victus/render/environment_map.hpp"

#include "cgltf.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <tuple>
#include <set>
#include <utility>

namespace victus::assets {

// ---- settings ----------------------------------------------------------------------------------

json::Value TextureImportSettings::to_json() const {
    json::Value v = json::Value::object();
    v.set("srgb", srgb);
    v.set("mipmaps", mipmaps);
    v.set("max_size", max_size);
    v.set("flip_y", flip_y);
    v.set("filter", nearest ? "nearest" : "linear");
    if (compression != "none") v.set("compression", compression);
    if (compression_thorough) v.set("compression_thorough", true);
    if (alpha_coverage_cutoff > 0.0f) v.set("alpha_coverage_cutoff", alpha_coverage_cutoff);
    return v;
}

TextureImportSettings TextureImportSettings::from_json(const json::Value& v) {
    TextureImportSettings s;
    s.srgb = v["srgb"].as_bool(s.srgb);
    s.mipmaps = v["mipmaps"].as_bool(s.mipmaps);
    s.max_size = static_cast<std::uint32_t>(std::clamp<std::int64_t>(v["max_size"].as_int(s.max_size), 1, 16384));
    s.flip_y = v["flip_y"].as_bool(s.flip_y);
    s.nearest = v["filter"].as_string() == "nearest";
    if (v["compression"].is_string()) s.compression = v["compression"].as_string();
    s.compression_thorough = v["compression_thorough"].as_bool(false);
    s.alpha_coverage_cutoff = v["alpha_coverage_cutoff"].as_float(0.0f);
    if (!(s.alpha_coverage_cutoff > 0.0f && s.alpha_coverage_cutoff < 1.0f)) s.alpha_coverage_cutoff = 0.0f;
    return s;
}

json::Value ModelImportSettings::to_json() const {
    json::Value v = json::Value::object();
    v.set("scale", scale);
    v.set("generate_tangents", generate_tangents);
    v.set("import_textures", import_textures);
    v.set("max_texture_size", max_texture_size);
    if (texture_compression != "none") v.set("texture_compression", texture_compression);
    if (rgba8_max_size != 0) v.set("rgba8_max_size", static_cast<std::int64_t>(rgba8_max_size));
    // Written only when changed, so existing .meta files stay as they are.
    if (!optimize_meshes) v.set("optimize_meshes", false);
    if (lods != ModelImportSettings{}.lods) v.set("lods", lods);
    if (lod_error != ModelImportSettings{}.lod_error) v.set("lod_error", lod_error);
    if (!import_lights) v.set("import_lights", false);
    if (!import_cameras) v.set("import_cameras", false);
    if (light_intensity_scale != ModelImportSettings{}.light_intensity_scale) v.set("light_intensity_scale", light_intensity_scale);
    if (!keyframe_reduction) v.set("keyframe_reduction", false);
    if (keyframe_rotation_error != ModelImportSettings{}.keyframe_rotation_error) v.set("keyframe_rotation_error", keyframe_rotation_error);
    if (keyframe_error != ModelImportSettings{}.keyframe_error) v.set("keyframe_error", keyframe_error);
    if (!skeleton_migrations.empty()) {
        json::Value a = json::Value::array();
        for (const std::string& m : skeleton_migrations) a.as_array().emplace_back(m);
        v.set("skeleton_migrations", std::move(a));
    }
    return v;
}

ModelImportSettings ModelImportSettings::from_json(const json::Value& v) {
    ModelImportSettings s;
    s.scale = v["scale"].as_float(s.scale);
    if (!(s.scale > 0.0f)) s.scale = 1.0f;
    s.generate_tangents = v["generate_tangents"].as_bool(s.generate_tangents);
    s.import_textures = v["import_textures"].as_bool(s.import_textures);
    s.max_texture_size =
        static_cast<std::uint32_t>(std::clamp<std::int64_t>(v["max_texture_size"].as_int(s.max_texture_size), 1, 16384));
    if (v["texture_compression"].is_string()) s.texture_compression = v["texture_compression"].as_string();
    s.rgba8_max_size = static_cast<std::uint32_t>(std::clamp<std::int64_t>(v["rgba8_max_size"].as_int(0), 0, 16384));
    s.optimize_meshes = v["optimize_meshes"].as_bool(s.optimize_meshes);
    s.lods = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(v["lods"].as_int(s.lods), 0, static_cast<std::int64_t>(render::kMaxMeshLods) - 1));
    s.lod_error = v["lod_error"].as_float(s.lod_error);
    if (!(s.lod_error > 0.0f)) s.lod_error = 0.05f;
    s.lod_error = std::min(s.lod_error, 1.0f);
    s.import_lights = v["import_lights"].as_bool(s.import_lights);
    s.import_cameras = v["import_cameras"].as_bool(s.import_cameras);
    s.light_intensity_scale = v["light_intensity_scale"].as_float(s.light_intensity_scale);
    if (!(s.light_intensity_scale >= 0.0f) || !std::isfinite(s.light_intensity_scale)) s.light_intensity_scale = 1.0f / 683.0f;
    s.keyframe_reduction = v["keyframe_reduction"].as_bool(s.keyframe_reduction);
    s.keyframe_rotation_error = std::clamp(v["keyframe_rotation_error"].as_float(s.keyframe_rotation_error), 0.0f, 10.0f);
    if (!std::isfinite(s.keyframe_rotation_error)) s.keyframe_rotation_error = ModelImportSettings{}.keyframe_rotation_error;
    s.keyframe_error = std::clamp(v["keyframe_error"].as_float(s.keyframe_error), 0.0f, 0.1f);
    if (!std::isfinite(s.keyframe_error)) s.keyframe_error = ModelImportSettings{}.keyframe_error;
    if (v["skeleton_migrations"].is_array())
        for (const json::Value& m : v["skeleton_migrations"].as_array())
            if (m.is_string() && !m.as_string().empty() && s.skeleton_migrations.size() < 64) s.skeleton_migrations.push_back(m.as_string());
    return s;
}

// ---- textures ----------------------------------------------------------------------------------

CookedTexture cook_texture(const render::Image& image, std::string name, bool srgb, bool mipmaps, std::uint32_t max_size,
                           float alpha_coverage_cutoff) {
    CookedTexture t;
    t.name = std::move(name);
    t.srgb = srgb;
    const bool oversized = image.width > max_size || image.height > max_size;
    if (mipmaps || oversized) {
        t.mips = render::build_mip_chain(image, srgb);
        // Coverage is matched to the full-resolution source, before oversized levels are dropped.
        if (mipmaps) render::preserve_alpha_coverage(t.mips, alpha_coverage_cutoff);
        std::size_t drop = 0;
        while (drop + 1 < t.mips.size() && (t.mips[drop].width > max_size || t.mips[drop].height > max_size)) ++drop;
        t.mips.erase(t.mips.begin(), t.mips.begin() + static_cast<std::ptrdiff_t>(drop));
        if (!mipmaps) t.mips.resize(1);
    } else {
        t.mips.push_back(image);
    }
    return t;
}

CookedTexture cook_hdr_texture(const render::ImageF& source, std::string name, std::uint32_t max_size) {
    const std::uint32_t limit = std::clamp(max_size, 4u, 8192u);
    const render::ImageF image = render::downsample_to_fit(source, limit, std::max(2u, limit / 2));
    CookedTexture t;
    t.name = std::move(name);
    t.srgb = true;
    t.hdr = true;
    t.hdr_width = image.width;
    t.hdr_height = image.height;
    t.hdr_pixels.resize(image.pixels.size());
    for (std::size_t i = 0; i < image.pixels.size(); ++i) t.hdr_pixels[i] = render::float_to_half(image.pixels[i]);
    const auto sh = render::SphericalHarmonics9::project(image);
    t.sh = sh.coefficients;
    // LDR preview (Reinhard, sRGB) at most 1024 wide.
    const render::ImageF small = render::downsample_to_fit(image, 1024, 512);
    render::Image ldr;
    ldr.width = small.width;
    ldr.height = small.height;
    ldr.pixels.resize(std::size_t{small.width} * small.height * 4);
    for (std::size_t i = 0; i < std::size_t{small.width} * small.height; ++i) {
        for (int c = 0; c < 3; ++c) {
            const float v = small.pixels[i * 4 + c];
            const float m = v / (1.0f + v);
            const float e = m <= 0.0031308f ? m * 12.92f : 1.055f * std::pow(m, 1.0f / 2.4f) - 0.055f;
            ldr.pixels[i * 4 + c] = static_cast<std::uint8_t>(std::lround(std::clamp(e, 0.0f, 1.0f) * 255.0f));
        }
        ldr.pixels[i * 4 + 3] = 255;
    }
    t.mips = render::build_mip_chain(ldr, true);
    return t;
}

void strip_rgba8_levels(CookedTexture& texture, std::uint32_t max_size) {
    if (max_size == 0 || texture.hdr || texture.mips.empty()) return;
    const bool covered = std::any_of(texture.compressed.begin(), texture.compressed.end(),
                                     [&](const CompressedMips& v) { return v.levels.size() == texture.mips.size(); });
    if (!covered) return;
    for (auto& m : texture.mips) {
        if (std::max(m.width, m.height) <= max_size) continue;
        m.pixels.clear();
        m.pixels.shrink_to_fit();
    }
}

Result<void> compress_cooked_texture(CookedTexture& texture, std::string_view setting, CompressionEffort effort) {
    auto codecs = parse_compression_setting(setting);
    if (!codecs) return std::move(codecs).error();
    texture.compressed.clear();
    if (texture.hdr || texture.mips.empty()) return {};
    for (const auto codec : *codecs) {
        auto mips = compress_mips(texture.mips, codec, texture.srgb, effort);
        if (!mips) return std::move(mips).error();
        texture.compressed.push_back(std::move(*mips));
    }
    return {};
}

Result<CookedTexture> import_texture(const fs::Path& path, const TextureImportSettings& settings) {
    if (auto codecs = parse_compression_setting(settings.compression); !codecs)
        return make_error(ErrorCode::InvalidArgument, std::format("{}: {}", path.string(), codecs.error().message));
    {
        auto bytes = fs::read_binary(path);
        if (!bytes) return std::move(bytes).error();
        if (render::is_hdr_image(*bytes)) {
            auto hdr = render::decode_image_hdr(*bytes);
            if (!hdr) return make_error(ErrorCode::ParseError, std::format("{}: {}", path.string(), hdr.error().message));
            if (settings.flip_y) {
                const std::size_t row = std::size_t{hdr->width} * 4;
                for (std::uint32_t y = 0; y < hdr->height / 2; ++y)
                    std::swap_ranges(hdr->pixels.begin() + static_cast<std::ptrdiff_t>(y * row),
                                     hdr->pixels.begin() + static_cast<std::ptrdiff_t>((y + 1) * row),
                                     hdr->pixels.begin() + static_cast<std::ptrdiff_t>((hdr->height - 1 - y) * row));
            }
            return cook_hdr_texture(*hdr, path.filename().string(), settings.max_size);
        }
    }
    auto img = render::load_image(path);
    if (!img) return std::move(img).error();
    render::Image& image = *img;
    if (settings.flip_y) {
        const std::size_t row = std::size_t{image.width} * 4;
        for (std::uint32_t y = 0; y < image.height / 2; ++y)
            std::swap_ranges(image.pixels.begin() + static_cast<std::ptrdiff_t>(y * row),
                             image.pixels.begin() + static_cast<std::ptrdiff_t>((y + 1) * row),
                             image.pixels.begin() + static_cast<std::ptrdiff_t>((image.height - 1 - y) * row));
    }
    CookedTexture t = cook_texture(image, path.filename().string(), settings.srgb, settings.mipmaps, settings.max_size,
                                   settings.alpha_coverage_cutoff);
    if (auto r = compress_cooked_texture(t, settings.compression,
                                         settings.compression_thorough ? CompressionEffort::Thorough : CompressionEffort::Fast);
        !r)
        return make_error(r.error().code, std::format("{}: {}", path.string(), r.error().message));
    return t;
}

// ---- glTF ----------------------------------------------------------------------------------------

namespace {

// Builds a path from UTF-8 bytes without relying on the narrow (ANSI on Windows) code page.
fs::Path utf8_path(std::string_view bytes) {
    std::u8string u;
    u.reserve(bytes.size());
    for (const char c : bytes) u.push_back(static_cast<char8_t>(c));
    return fs::Path(u);
}

struct ReadContext {
    fs::Path sandbox;
    std::vector<fs::Path>* dependencies;
    std::string error;
    bool memory_only{false}; // import_gltf_memory: every external reference fails
};

bool inside(const fs::Path& root, const fs::Path& p) {
    std::error_code ec;
    const fs::Path r = std::filesystem::weakly_canonical(root, ec);
    if (ec) return false;
    const fs::Path c = std::filesystem::weakly_canonical(p, ec);
    if (ec) return false;
    auto ri = r.begin();
    auto ci = c.begin();
    for (; ri != r.end(); ++ri, ++ci) {
        if (ci == c.end() || *ri != *ci) return false;
    }
    return true;
}

cgltf_result sandboxed_read(const cgltf_memory_options*, const cgltf_file_options* options, const char* path,
                            cgltf_size* size, void** data) {
    auto* ctx = static_cast<ReadContext*>(options->user_data);
    const std::string_view sv(path);
    if (ctx->memory_only) {
        ctx->error = std::format("external reference in an in-memory glTF: {}", sv);
        return cgltf_result_file_not_found;
    }
    if (sv.find("://") != std::string_view::npos) {
        ctx->error = std::format("remote URI not allowed: {}", sv);
        return cgltf_result_file_not_found;
    }
    const fs::Path p = utf8_path(sv);
    if (!inside(ctx->sandbox, p)) {
        ctx->error = std::format("glTF reference escapes the asset directory: {}", sv);
        return cgltf_result_file_not_found;
    }
    auto bytes = fs::read_binary(p, 1ull << 31);
    if (!bytes) {
        ctx->error = bytes.error().message;
        return cgltf_result_file_not_found;
    }
    void* mem = std::malloc(std::max<std::size_t>(bytes->size(), 1));
    if (!mem) return cgltf_result_out_of_memory;
    if (!bytes->empty()) std::memcpy(mem, bytes->data(), bytes->size());
    *size = bytes->size();
    *data = mem;
    if (ctx->dependencies) ctx->dependencies->push_back(p);
    return cgltf_result_success;
}

void sandboxed_release(const cgltf_memory_options*, const cgltf_file_options*, void* data) { std::free(data); }

// Overflow-checked bounds validation of every buffer view and accessor, run before
// cgltf_validate(). cgltf <= 1.15 computes these sizes with unchecked arithmetic
// (CVE-2026-32845, CVE-2026-75148), so crafted counts or offsets could pass its checks and lead
// to out-of-bounds reads. Sparse indices are checked against the accessor count here too.
constexpr std::uint64_t kU64Max = ~std::uint64_t{0};
bool add_ok(std::uint64_t a, std::uint64_t b, std::uint64_t& out) {
    if (a > kU64Max - b) return false;
    out = a + b;
    return true;
}
bool mul_ok(std::uint64_t a, std::uint64_t b, std::uint64_t& out) {
    if (a != 0 && b > kU64Max / a) return false;
    out = a * b;
    return true;
}

// Largest element count accepted for one accessor. Far above any real asset (the importer reads
// at most 2 GiB files) and keeps count * components * sizeof(float) far from overflowing.
constexpr std::uint64_t kMaxAccessorCount = 1ull << 28;

bool range_fits(std::uint64_t offset, std::uint64_t length, std::uint64_t size) {
    std::uint64_t end = 0;
    return add_ok(offset, length, end) && end <= size;
}

Result<void> validate_accessors(const cgltf_data& data) {
    for (cgltf_size i = 0; i < data.buffer_views_count; ++i) {
        const cgltf_buffer_view& v = data.buffer_views[i];
        if (!v.buffer) return make_error(ErrorCode::ParseError, std::format("bufferView {} has no buffer", i));
        if (!range_fits(v.offset, v.size, v.buffer->size))
            return make_error(ErrorCode::ParseError, std::format("bufferView {} exceeds its buffer", i));
        if (v.stride != 0 && (v.stride < 4 || v.stride > 252))
            return make_error(ErrorCode::ParseError, std::format("bufferView {} has invalid byteStride {}", i, v.stride));
    }
    for (cgltf_size i = 0; i < data.accessors_count; ++i) {
        const cgltf_accessor& a = data.accessors[i];
        if (a.component_type == cgltf_component_type_invalid || a.type == cgltf_type_invalid)
            return make_error(ErrorCode::ParseError, std::format("accessor {} has an invalid type", i));
        if (a.count == 0 || a.count > kMaxAccessorCount)
            return make_error(ErrorCode::ParseError, std::format("accessor {} has invalid count {}", i, a.count));
        const std::uint64_t element = cgltf_calc_size(a.type, a.component_type);
        if (a.buffer_view) {
            std::uint64_t span = 0;
            if (a.stride < element || !mul_ok(a.stride, a.count - 1, span) || !add_ok(span, element, span) ||
                !range_fits(a.offset, span, a.buffer_view->size))
                return make_error(ErrorCode::ParseError, std::format("accessor {} reads outside its bufferView", i));
        }
        if (!a.is_sparse) continue;
        const cgltf_accessor_sparse& sp = a.sparse;
        if (sp.count == 0 || sp.count > a.count)
            return make_error(ErrorCode::ParseError, std::format("accessor {}: invalid sparse count {}", i, sp.count));
        if (sp.indices_component_type != cgltf_component_type_r_8u && sp.indices_component_type != cgltf_component_type_r_16u &&
            sp.indices_component_type != cgltf_component_type_r_32u)
            return make_error(ErrorCode::ParseError, std::format("accessor {}: invalid sparse index type", i));
        if (!sp.indices_buffer_view || !sp.values_buffer_view || !sp.indices_buffer_view->buffer->data ||
            !sp.values_buffer_view->buffer->data)
            return make_error(ErrorCode::ParseError, std::format("accessor {}: sparse data is missing", i));
        const std::uint64_t index_size = cgltf_component_size(sp.indices_component_type);
        std::uint64_t indices_len = 0, values_len = 0;
        if (!mul_ok(index_size, sp.count, indices_len) || !range_fits(sp.indices_byte_offset, indices_len, sp.indices_buffer_view->size) ||
            !mul_ok(element, sp.count, values_len) || !range_fits(sp.values_byte_offset, values_len, sp.values_buffer_view->size))
            return make_error(ErrorCode::ParseError, std::format("accessor {}: sparse data exceeds its bufferView", i));
        const std::uint8_t* idx = static_cast<const std::uint8_t*>(sp.indices_buffer_view->buffer->data) +
                                  sp.indices_buffer_view->offset + sp.indices_byte_offset;
        for (cgltf_size k = 0; k < sp.count; ++k) {
            std::uint64_t value = 0;
            const std::uint8_t* p = idx + k * index_size;
            for (std::uint64_t b = 0; b < index_size; ++b) value |= std::uint64_t{p[b]} << (8 * b);
            if (value >= a.count)
                return make_error(ErrorCode::ParseError,
                                  std::format("accessor {}: sparse index {} is outside its {} elements", i, value, a.count));
        }
    }
    return {};
}

const char* result_name(cgltf_result r) {
    switch (r) {
    case cgltf_result_data_too_short: return "data too short";
    case cgltf_result_unknown_format: return "unknown format";
    case cgltf_result_invalid_json: return "invalid JSON";
    case cgltf_result_invalid_gltf: return "invalid glTF";
    case cgltf_result_invalid_options: return "invalid options";
    case cgltf_result_file_not_found: return "file not found";
    case cgltf_result_io_error: return "I/O error";
    case cgltf_result_out_of_memory: return "out of memory";
    case cgltf_result_legacy_gltf: return "legacy glTF 1.0 is not supported";
    default: return "error";
    }
}

struct DataGuard {
    cgltf_data* data{nullptr};
    ~DataGuard() {
        if (data) cgltf_free(data);
    }
};

class GltfImporter {
public:
    GltfImporter(const fs::Path& path, const ModelImportSettings& settings, cgltf_options& options, ReadContext& ctx,
                 ImportResult& result)
        : path_(path), settings_(settings), options_(options), ctx_(ctx), result_(result) {}

    Result<CookedModel> run(cgltf_data* data) {
        data_ = data;
        // Meshes: one ModelMesh per triangle primitive.
        mesh_map_.resize(data->meshes_count);
        for (cgltf_size mi = 0; mi < data->meshes_count; ++mi) {
            const cgltf_mesh& mesh = data->meshes[mi];
            for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi) {
                auto r = import_primitive(mesh, pi);
                if (!r) return std::move(r).error();
                if (r->data.vertices.empty()) continue;
                if (settings_.optimize_meshes) {
                    render::LodSettings lod;
                    lod.max_lods = settings_.lods;
                    lod.max_error = settings_.lod_error;
                    render::generate_lods(r->data, lod);
                    render::optimize_mesh(r->data);
                }
                mesh_map_[mi].push_back(static_cast<std::uint32_t>(model_.meshes.size()));
                model_.meshes.push_back(std::move(r).value());
            }
        }
        if (model_.materials.empty()) default_material();
        // Nodes: depth-first from the scene roots so parents precede children.
        const cgltf_scene* scene = data->scene ? data->scene : (data->scenes_count ? &data->scenes[0] : nullptr);
        if (scene) {
            for (cgltf_size i = 0; i < scene->nodes_count; ++i) add_node(scene->nodes[i], -1, 0);
        } else {
            for (cgltf_size i = 0; i < data->nodes_count; ++i)
                if (!data->nodes[i].parent) add_node(&data->nodes[i], -1, 0);
        }
        if (model_.nodes.empty()) {
            // No scene graph: expose every mesh at the origin.
            for (cgltf_size mi = 0; mi < data->meshes_count; ++mi) {
                ModelNode n;
                n.name = data->meshes[mi].name ? data->meshes[mi].name : std::format("mesh{}", mi);
                n.meshes = mesh_map_[mi];
                model_.nodes.push_back(std::move(n));
            }
        }
        for (auto& n : model_.nodes) {
            if (n.parent < 0) {
                n.translation = n.translation * settings_.scale;
                n.scale = n.scale * settings_.scale;
            }
        }
        import_skins();
        import_animations();
        // Scenes of only lights/cameras (lighting rigs, camera paths) are valid models.
        if (model_.meshes.empty() && model_.lights.empty() && model_.cameras.empty())
            return make_error(ErrorCode::InvalidArgument, "glTF contains no triangle meshes, lights or cameras");
        if (auto st = validate_model(model_); !st)
            return make_error(ErrorCode::ParseError, std::format("{}: {}", path_.filename().string(), st.error().message));
        return std::move(model_);
    }

private:
    void warn(std::string w) { result_.warnings.push_back(std::move(w)); }

    std::uint32_t default_material() {
        if (default_material_ < 0) {
            ModelMaterial m;
            m.name = "Default";
            m.metallic = 0.0f;
            m.roughness = 0.5f;
            default_material_ = static_cast<std::int32_t>(model_.materials.size());
            model_.materials.push_back(m);
        }
        return static_cast<std::uint32_t>(default_material_);
    }

    Result<render::Image> load_image(const cgltf_image& image) {
        std::vector<std::uint8_t> bytes;
        if (image.buffer_view) {
            const cgltf_buffer_view& bv = *image.buffer_view;
            if (!bv.buffer || !bv.buffer->data || bv.offset + bv.size > bv.buffer->size)
                return make_error(ErrorCode::ParseError, "image buffer view out of range");
            const auto* p = static_cast<const std::uint8_t*>(bv.buffer->data) + bv.offset;
            bytes.assign(p, p + bv.size);
        } else if (image.uri) {
            const std::string_view uri(image.uri);
            if (uri.starts_with("data:")) {
                const auto comma = uri.find(";base64,");
                if (comma == std::string_view::npos) return make_error(ErrorCode::Unsupported, "non-base64 data URI image");
                const std::string_view b64 = uri.substr(comma + 8);
                std::size_t padding = 0;
                if (b64.ends_with("==")) padding = 2;
                else if (b64.ends_with("=")) padding = 1;
                const cgltf_size size = b64.size() / 4 * 3 - padding;
                void* out = nullptr;
                if (cgltf_load_buffer_base64(&options_, size, std::string(b64).c_str(), &out) != cgltf_result_success)
                    return make_error(ErrorCode::ParseError, "invalid base64 image");
                bytes.assign(static_cast<std::uint8_t*>(out), static_cast<std::uint8_t*>(out) + size);
                std::free(out);
            } else {
                std::string decoded(uri);
                cgltf_decode_uri(decoded.data());
                decoded.resize(std::strlen(decoded.c_str()));
                const fs::Path full = path_.parent_path() / utf8_path(decoded);
                cgltf_size size = 0;
                void* data = nullptr;
                const std::string full_s = full.string();
                if (sandboxed_read(nullptr, &options_.file, full_s.c_str(), &size, &data) != cgltf_result_success)
                    return make_error(ErrorCode::NotFound, ctx_.error);
                bytes.assign(static_cast<std::uint8_t*>(data), static_cast<std::uint8_t*>(data) + size);
                std::free(data);
            }
        } else {
            return make_error(ErrorCode::ParseError, "image has neither uri nor bufferView");
        }
        return render::decode_image(bytes);
    }

    // `alpha_cutoff` > 0: the texture feeds an alpha-tested (MASK) material, so its mips keep the
    // alpha-test coverage of the source (render::preserve_alpha_coverage).
    std::int32_t texture(const cgltf_texture_view& view, bool srgb, const char* slot, float alpha_cutoff = 0.0f) {
        if (!view.texture || !settings_.import_textures) return -1;
        const cgltf_image* image = view.texture->image;
        if (!image) {
            warn(std::format("{} texture uses an unsupported image extension (e.g. KTX2/WebP); skipped", slot));
            return -1;
        }
        if (view.texcoord != 0) warn(std::format("{} texture uses TEXCOORD_{}; only TEXCOORD_0 is supported", slot, view.texcoord));
        const auto key = std::make_tuple(image, srgb, alpha_cutoff);
        if (const auto it = textures_.find(key); it != textures_.end()) return it->second;
        auto img = load_image(*image);
        std::int32_t index = -1;
        if (!img) {
            warn(std::format("{} texture '{}': {}", slot, image->name ? image->name : (image->uri ? image->uri : "?"),
                             img.error().message));
        } else {
            std::string name = image->name ? image->name : std::format("image{}", image - data_->images);
            index = static_cast<std::int32_t>(model_.textures.size());
            CookedTexture t = cook_texture(*img, std::move(name), srgb, true, settings_.max_texture_size, alpha_cutoff);
            if (auto r = compress_cooked_texture(t, settings_.texture_compression); !r)
                warn(std::format("{} texture '{}': {}", slot, t.name, r.error().message));
            strip_rgba8_levels(t, settings_.rgba8_max_size);
            model_.textures.push_back(std::move(t));
        }
        textures_.emplace(key, index);
        return index;
    }

    // glTF occlusion (ADR-0139): VICTUS reads ambient occlusion from the red channel of the
    // metallic-roughness texture, glTF's packed ORM layout. A shared ORM image is used as it is;
    // a separate occlusion image is merged into the metallic-roughness one (or, without one, into
    // a texture whose roughness and metallic channels are 1, leaving the factors unchanged).
    [[nodiscard]] bool separate_occlusion(const cgltf_material& m) const {
        const cgltf_texture_view& occ = m.occlusion_texture;
        const cgltf_texture_view& mr = m.pbr_metallic_roughness.metallic_roughness_texture;
        return settings_.import_textures && occ.texture && occ.texture->image && mr.texture && mr.texture->image &&
               occ.texture->image != mr.texture->image;
    }
    void occlusion(const cgltf_texture_view& occ, const cgltf_texture_view* mr, ModelMaterial& out) {
        if (!occ.texture || !settings_.import_textures) return;
        const cgltf_image* occ_image = occ.texture->image;
        const cgltf_image* mr_image = mr && mr->texture ? mr->texture->image : nullptr;
        if (!occ_image) {
            warn("occlusion texture uses an unsupported image extension (e.g. KTX2/WebP); skipped");
            if (mr_image) out.metallic_roughness_texture = texture(*mr, false, "metallicRoughness");
            return;
        }
        if (occ.texcoord != 0) warn(std::format("occlusion texture uses TEXCOORD_{}; only TEXCOORD_0 is supported", occ.texcoord));
        const float strength = std::clamp(occ.scale, 0.0f, 1.0f);
        if (occ_image == mr_image) {
            if (out.metallic_roughness_texture >= 0) out.occlusion_strength = strength;
            return;
        }
        const auto key = std::make_pair(occ_image, mr_image);
        if (const auto it = orm_textures_.find(key); it != orm_textures_.end()) {
            if (it->second >= 0) {
                out.metallic_roughness_texture = it->second;
                out.occlusion_strength = strength;
            } else if (mr_image) {
                out.metallic_roughness_texture = texture(*mr, false, "metallicRoughness");
            }
            return;
        }
        std::int32_t index = -1;
        auto ao = load_image(*occ_image);
        if (!ao) {
            warn(std::format("occlusion texture: {}", ao.error().message));
        } else {
            render::Image orm;
            bool ok = true;
            if (mr_image) {
                auto base = load_image(*mr_image);
                if (!base || base->width != ao->width || base->height != ao->height) {
                    warn("occlusion texture differs in size from the metallic-roughness texture; occlusion skipped");
                    ok = false;
                } else {
                    orm = std::move(*base);
                }
            } else {
                orm.width = ao->width;
                orm.height = ao->height;
                orm.pixels.assign(ao->pixels.size(), 255);
            }
            if (ok) {
                for (std::size_t i = 0; i < orm.pixels.size(); i += 4) orm.pixels[i] = ao->pixels[i];
                std::string name = (occ_image->name ? std::string(occ_image->name) : std::format("image{}", occ_image - data_->images)) + "_orm";
                CookedTexture t = cook_texture(orm, std::move(name), false, true, settings_.max_texture_size);
                if (auto r = compress_cooked_texture(t, settings_.texture_compression); !r)
                    warn(std::format("occlusion texture '{}': {}", t.name, r.error().message));
                strip_rgba8_levels(t, settings_.rgba8_max_size);
                index = static_cast<std::int32_t>(model_.textures.size());
                model_.textures.push_back(std::move(t));
            }
        }
        orm_textures_.emplace(key, index);
        if (index >= 0) {
            out.metallic_roughness_texture = index;
            out.occlusion_strength = strength;
        } else if (mr_image) {
            out.metallic_roughness_texture = texture(*mr, false, "metallicRoughness"); // without occlusion
        }
    }

    std::uint32_t material(const cgltf_material* m) {
        if (!m) return default_material();
        if (const auto it = materials_.find(m); it != materials_.end()) return it->second;
        ModelMaterial out;
        out.name = m->name ? m->name : std::format("material{}", m - data_->materials);
        switch (m->alpha_mode) {
        case cgltf_alpha_mode_mask: out.alpha_cutoff = m->alpha_cutoff; break;
        case cgltf_alpha_mode_blend:
            out.alpha_cutoff = 0.5f;
            warn(out.name + ": alpha blending is rendered as alpha-tested (cutoff 0.5) until the transparency pass lands");
            break;
        default: out.alpha_cutoff = 0.0f; break;
        }
        if (m->has_pbr_metallic_roughness) {
            const auto& pbr = m->pbr_metallic_roughness;
            out.base_color = {pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2], pbr.base_color_factor[3]};
            out.metallic = pbr.metallic_factor;
            out.roughness = pbr.roughness_factor;
            out.base_color_texture = texture(pbr.base_color_texture, true, "baseColor", out.alpha_cutoff);
            // A separate occlusion image is merged into this one by occlusion() below.
            if (!separate_occlusion(*m)) out.metallic_roughness_texture = texture(pbr.metallic_roughness_texture, false, "metallicRoughness");
        } else {
            out.metallic = 0.0f;
            out.roughness = 0.5f;
            if (m->has_pbr_specular_glossiness) warn(out.name + ": KHR_materials_pbrSpecularGlossiness approximated by defaults");
        }
        occlusion(m->occlusion_texture, m->has_pbr_metallic_roughness ? &m->pbr_metallic_roughness.metallic_roughness_texture : nullptr,
                  out);
        out.normal_texture = texture(m->normal_texture, false, "normal");
        out.normal_scale = m->normal_texture.texture ? m->normal_texture.scale : 1.0f;
        out.emissive = {m->emissive_factor[0], m->emissive_factor[1], m->emissive_factor[2], 1.0f};
        out.emissive_texture = texture(m->emissive_texture, true, "emissive");
        if (out.emissive_texture >= 0 && out.emissive.r == 0 && out.emissive.g == 0 && out.emissive.b == 0)
            warn(out.name + ": emissive texture with zero emissiveFactor has no effect");
        out.emissive_strength = m->has_emissive_strength ? m->emissive_strength.emissive_strength : 1.0f;
        if (m->has_clearcoat) {
            const auto& cc = m->clearcoat;
            out.clearcoat = std::clamp(cc.clearcoat_factor, 0.0f, 1.0f);
            out.clearcoat_roughness = std::clamp(cc.clearcoat_roughness_factor, 0.0f, 1.0f);
            if (cc.clearcoat_texture.texture || cc.clearcoat_roughness_texture.texture || cc.clearcoat_normal_texture.texture)
                warn(out.name + ": KHR_materials_clearcoat textures are ignored (the coat uses its factors)");
        }
        out.double_sided = m->double_sided;
        out.unlit = m->unlit;
        const auto index = static_cast<std::uint32_t>(model_.materials.size());
        model_.materials.push_back(std::move(out));
        materials_.emplace(m, index);
        return index;
    }

    // Accessors were bounds-checked by validate_accessors() before this runs. Sparse accessors are
    // applied here instead of by cgltf_accessor_unpack_floats, which in cgltf <= 1.15 advances the
    // sparse values by the base view's byteStride (CVE-2026-112019) and does not bound sparse indices
    // (CVE-2026-112018). Sparse values are tightly packed (glTF 2.0 3.6.2.3).
    static std::vector<float> unpack(const cgltf_accessor* acc) {
        const cgltf_size comps = cgltf_num_components(acc->type);
        const cgltf_size n = acc->count * comps;
        std::vector<float> out(n);
        if (!acc->is_sparse) {
            cgltf_accessor_unpack_floats(acc, out.data(), n);
            return out;
        }
        cgltf_accessor base = *acc;
        base.is_sparse = 0;
        cgltf_accessor_unpack_floats(&base, out.data(), n);
        const cgltf_accessor_sparse& sp = acc->sparse;
        cgltf_accessor indices{};
        indices.component_type = sp.indices_component_type;
        indices.type = cgltf_type_scalar;
        indices.count = sp.count;
        indices.buffer_view = sp.indices_buffer_view;
        indices.offset = sp.indices_byte_offset;
        indices.stride = cgltf_component_size(sp.indices_component_type);
        cgltf_accessor values{};
        values.component_type = acc->component_type;
        values.normalized = acc->normalized;
        values.type = acc->type;
        values.count = sp.count;
        values.buffer_view = sp.values_buffer_view;
        values.offset = sp.values_byte_offset;
        values.stride = cgltf_calc_size(acc->type, acc->component_type);
        for (cgltf_size i = 0; i < sp.count; ++i) {
            const cgltf_size target = cgltf_accessor_read_index(&indices, i);
            if (target >= acc->count) continue; // rejected by validate_accessors(); defensive
            cgltf_accessor_read_float(&values, i, out.data() + target * comps, comps);
        }
        return out;
    }

    Result<ModelMesh> import_primitive(const cgltf_mesh& mesh, cgltf_size pi) {
        const cgltf_primitive& prim = mesh.primitives[pi];
        ModelMesh out;
        out.name = std::format("{}#{}", mesh.name ? mesh.name : "mesh", pi);
        if (prim.type != cgltf_primitive_type_triangles && prim.type != cgltf_primitive_type_triangle_strip &&
            prim.type != cgltf_primitive_type_triangle_fan) {
            warn(out.name + ": non-triangle primitive skipped");
            return out;
        }
        if (prim.has_draco_mesh_compression) {
            warn(out.name + ": KHR_draco_mesh_compression is not supported; primitive skipped");
            return out;
        }
        const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr, *tan = nullptr, *col = nullptr;
        const cgltf_accessor *joints[2] = {nullptr, nullptr}, *weights[2] = {nullptr, nullptr};
        for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
            const cgltf_attribute& attr = prim.attributes[a];
            switch (attr.type) {
            case cgltf_attribute_type_joints:
                if (attr.index < 2) joints[attr.index] = attr.data;
                else warn(std::format("{}: JOINTS_{} ignored (up to 8 influences are read, 4 kept)", out.name, attr.index));
                break;
            case cgltf_attribute_type_weights:
                if (attr.index < 2) weights[attr.index] = attr.data;
                break;
            case cgltf_attribute_type_position: pos = attr.data; break;
            case cgltf_attribute_type_normal: nrm = attr.data; break;
            case cgltf_attribute_type_tangent: tan = attr.data; break;
            case cgltf_attribute_type_texcoord:
                if (attr.index == 0) uv = attr.data;
                break;
            case cgltf_attribute_type_color:
                if (attr.index == 0) col = attr.data;
                break;
            default: break;
            }
        }
        if (!pos || pos->type != cgltf_type_vec3) {
            warn(out.name + ": primitive without vec3 POSITION skipped");
            return out;
        }
        const cgltf_size count = pos->count;
        if (count == 0) return out;
        if (count > (1u << 26)) return make_error(ErrorCode::LimitExceeded, out.name + ": too many vertices");
        auto& verts = out.data.vertices;
        verts.resize(count);
        const auto p = unpack(pos);
        for (cgltf_size i = 0; i < count; ++i) verts[i].position = {p[i * 3], p[i * 3 + 1], p[i * 3 + 2]};
        if (nrm && nrm->type == cgltf_type_vec3 && nrm->count == count) {
            const auto n = unpack(nrm);
            for (cgltf_size i = 0; i < count; ++i) verts[i].normal = {n[i * 3], n[i * 3 + 1], n[i * 3 + 2]};
        }
        if (uv && uv->type == cgltf_type_vec2 && uv->count == count) {
            const auto t = unpack(uv);
            for (cgltf_size i = 0; i < count; ++i) verts[i].uv = {t[i * 2], t[i * 2 + 1]};
        }
        // COLOR_0 (linear in glTF; float or normalized ints, RGB or RGBA) -> sRGB-encoded RGBA8.
        if (col && (col->type == cgltf_type_vec3 || col->type == cgltf_type_vec4) && col->count == count) {
            const cgltf_size comps = col->type == cgltf_type_vec4 ? 4 : 3;
            for (cgltf_size i = 0; i < count; ++i) {
                float c[4]{1, 1, 1, 1};
                cgltf_accessor_read_float(col, i, c, comps);
                for (float& v : c) v = std::isfinite(v) ? v : 1.0f;
                verts[i].color = render::pack_vertex_color({c[0], c[1], c[2], c[3]});
            }
        } else if (col) {
            warn(out.name + ": COLOR_0 must be vec3/vec4 with one entry per vertex; ignored");
        }
        const bool has_tangents = tan && tan->type == cgltf_type_vec4 && tan->count == count;
        if (has_tangents) {
            const auto t = unpack(tan);
            for (cgltf_size i = 0; i < count; ++i) verts[i].tangent = {t[i * 4], t[i * 4 + 1], t[i * 4 + 2], t[i * 4 + 3]};
        }
        // Skin: up to two JOINTS_n/WEIGHTS_n sets, reduced to the 4 strongest normalized influences.
        if (joints[0] && weights[0]) {
            const cgltf_size sets = joints[1] && weights[1] ? 2 : 1;
            for (cgltf_size set = 0; set < sets; ++set)
                if (joints[set]->type != cgltf_type_vec4 || weights[set]->type != cgltf_type_vec4 || joints[set]->count != count ||
                    weights[set]->count != count)
                    return make_error(ErrorCode::ParseError, out.name + ": JOINTS/WEIGHTS must be vec4 with one entry per vertex");
            out.data.skin.resize(count);
            for (cgltf_size i = 0; i < count; ++i) {
                std::uint32_t j[8]{};
                float w[8]{};
                for (cgltf_size set = 0; set < sets; ++set) {
                    cgltf_uint ju[4]{};
                    float wf[4]{};
                    cgltf_accessor_read_uint(joints[set], i, ju, 4);
                    cgltf_accessor_read_float(weights[set], i, wf, 4);
                    for (int k = 0; k < 4; ++k) {
                        if (ju[k] > 65535) return make_error(ErrorCode::ParseError, out.name + ": joint index out of range");
                        j[set * 4 + static_cast<cgltf_size>(k)] = ju[k];
                        w[set * 4 + static_cast<cgltf_size>(k)] = std::isfinite(wf[k]) ? std::max(wf[k], 0.0f) : 0.0f;
                    }
                }
                out.data.skin[i] = render::make_skin_vertex(j, w, sets * 4);
            }
        } else if (joints[0] || weights[0]) {
            warn(out.name + ": JOINTS_0 without WEIGHTS_0 (or the reverse); skin ignored");
        }
        // Morph targets: POSITION/NORMAL deltas, stored sparsely (only vertices that move).
        if (prim.targets_count > render::kMaxMorphTargets)
            return make_error(ErrorCode::LimitExceeded, out.name + ": too many morph targets");
        for (cgltf_size t = 0; t < prim.targets_count; ++t) {
            const cgltf_morph_target& mt = prim.targets[t];
            const cgltf_accessor *dp = nullptr, *dn = nullptr;
            for (cgltf_size a = 0; a < mt.attributes_count; ++a) {
                const cgltf_attribute& attr = mt.attributes[a];
                if (attr.type == cgltf_attribute_type_position) dp = attr.data;
                else if (attr.type == cgltf_attribute_type_normal) dn = attr.data;
            }
            if (dp && (dp->type != cgltf_type_vec3 || dp->count != count)) dp = nullptr;
            if (dn && (dn->type != cgltf_type_vec3 || dn->count != count)) dn = nullptr;
            render::MorphTarget target;
            target.name = t < mesh.target_names_count && mesh.target_names[t] ? mesh.target_names[t]
                                                                               : std::format("target{}", t);
            const std::vector<float> pd = dp ? unpack(dp) : std::vector<float>{};
            const std::vector<float> nd = dn ? unpack(dn) : std::vector<float>{};
            for (cgltf_size i = 0; i < count; ++i) {
                const math::Vec3 dpos = dp ? math::Vec3{pd[i * 3], pd[i * 3 + 1], pd[i * 3 + 2]} : math::Vec3{};
                const math::Vec3 dnrm = dn ? math::Vec3{nd[i * 3], nd[i * 3 + 1], nd[i * 3 + 2]} : math::Vec3{};
                const bool finite = std::isfinite(dpos.x) && std::isfinite(dpos.y) && std::isfinite(dpos.z) &&
                                    std::isfinite(dnrm.x) && std::isfinite(dnrm.y) && std::isfinite(dnrm.z);
                if (!finite) return make_error(ErrorCode::ParseError, out.name + ": non-finite morph target delta");
                if (dpos.x == 0 && dpos.y == 0 && dpos.z == 0 && dnrm.x == 0 && dnrm.y == 0 && dnrm.z == 0) continue;
                target.vertices.push_back(static_cast<std::uint32_t>(i));
                target.positions.push_back(dpos);
                if (dn) target.normals.push_back(dnrm);
            }
            out.data.morphs.push_back(std::move(target));
        }
        if (!out.data.morphs.empty()) {
            out.data.morph_weights.assign(out.data.morphs.size(), 0.0f);
            for (cgltf_size t = 0; t < mesh.weights_count && t < out.data.morph_weights.size(); ++t)
                out.data.morph_weights[t] = std::isfinite(mesh.weights[t]) ? mesh.weights[t] : 0.0f;
        }
        // Indices, converting strips/fans to lists.
        std::vector<std::uint32_t> raw;
        if (prim.indices) {
            raw.resize(prim.indices->count);
            for (cgltf_size i = 0; i < raw.size(); ++i) raw[i] = static_cast<std::uint32_t>(cgltf_accessor_read_index(prim.indices, i));
        } else {
            raw.resize(count);
            for (cgltf_size i = 0; i < count; ++i) raw[i] = static_cast<std::uint32_t>(i);
        }
        for (const std::uint32_t i : raw)
            if (i >= count) return make_error(ErrorCode::ParseError, out.name + ": index out of range");
        auto& idx = out.data.indices;
        if (prim.type == cgltf_primitive_type_triangles) {
            idx = std::move(raw);
            idx.resize(idx.size() / 3 * 3);
        } else if (prim.type == cgltf_primitive_type_triangle_strip) {
            for (std::size_t i = 2; i < raw.size(); ++i) {
                if (i % 2 == 0) idx.insert(idx.end(), {raw[i - 2], raw[i - 1], raw[i]});
                else idx.insert(idx.end(), {raw[i - 1], raw[i - 2], raw[i]});
            }
        } else {
            for (std::size_t i = 2; i < raw.size(); ++i) idx.insert(idx.end(), {raw[0], raw[i - 1], raw[i]});
        }
        if (!nrm) render::compute_normals(out.data);
        if (!has_tangents && settings_.generate_tangents) render::compute_tangents(out.data);
        out.material = material(prim.material);
        return out;
    }

    void add_node(const cgltf_node* node, std::int32_t parent, int depth) {
        if (depth > 256) {
            warn("node hierarchy deeper than 256 levels truncated");
            return;
        }
        ModelNode n;
        n.name = node->name ? node->name : std::format("node{}", node - data_->nodes);
        n.parent = parent;
        if (node->has_matrix) {
            math::Mat4 m;
            std::memcpy(&m, node->matrix, sizeof(float) * 16);
            math::decompose(m, n.translation, n.rotation, n.scale);
        } else {
            if (node->has_translation) n.translation = {node->translation[0], node->translation[1], node->translation[2]};
            if (node->has_rotation)
                n.rotation = math::normalize(math::Quat{node->rotation[0], node->rotation[1], node->rotation[2], node->rotation[3]});
            if (node->has_scale) n.scale = {node->scale[0], node->scale[1], node->scale[2]};
        }
        if (node->mesh) n.meshes = mesh_map_[static_cast<std::size_t>(node->mesh - data_->meshes)];
        if (node->skin) n.skin = static_cast<std::int32_t>(node->skin - data_->skins); // remapped in import_skins
        for (cgltf_size t = 0; t < node->weights_count && t < render::kMaxMorphTargets; ++t)
            n.morph_weights.push_back(std::isfinite(node->weights[t]) ? node->weights[t] : 0.0f);
        if (node->light && settings_.import_lights) n.light = light(node->light);
        if (node->camera && settings_.import_cameras) n.camera = camera(node->camera);
        const auto index = static_cast<std::int32_t>(model_.nodes.size());
        node_map_[node] = static_cast<std::uint32_t>(index);
        model_.nodes.push_back(std::move(n));
        for (cgltf_size c = 0; c < node->children_count; ++c) add_node(node->children[c], index, depth + 1);
    }

    // KHR_lights_punctual: photometric intensity times light_intensity_scale; the range follows
    // the import scale (the spec leaves light properties unaffected by node scale).
    std::int32_t light(const cgltf_light* l) {
        if (auto it = light_map_.find(l); it != light_map_.end()) return it->second;
        const auto fin = [](float v, float d) { return std::isfinite(v) ? v : d; };
        ModelLight out;
        out.name = l->name ? l->name : std::format("light{}", l - data_->lights);
        out.type = l->type == cgltf_light_type_directional ? ModelLight::Type::Directional
                   : l->type == cgltf_light_type_spot      ? ModelLight::Type::Spot
                                                           : ModelLight::Type::Point;
        out.color = {std::max(fin(l->color[0], 1), 0.0f), std::max(fin(l->color[1], 1), 0.0f), std::max(fin(l->color[2], 1), 0.0f), 1.0f};
        out.intensity = std::max(fin(l->intensity, 1.0f), 0.0f) * settings_.light_intensity_scale;
        out.range = std::max(fin(l->range, 0.0f), 0.0f) * settings_.scale;
        if (out.type == ModelLight::Type::Spot) {
            constexpr float kHalfPi = 1.5707963f;
            out.outer_cone = std::clamp(fin(l->spot_outer_cone_angle, 0.7853982f), 0.001f, kHalfPi);
            out.inner_cone = std::clamp(fin(l->spot_inner_cone_angle, 0.0f), 0.0f, out.outer_cone);
        }
        const auto index = static_cast<std::int32_t>(model_.lights.size());
        model_.lights.push_back(std::move(out));
        light_map_[l] = index;
        return index;
    }

    std::int32_t camera(const cgltf_camera* c) {
        if (auto it = camera_map_.find(c); it != camera_map_.end()) return it->second;
        const auto fin = [](float v, float d) { return std::isfinite(v) && v > 0.0f ? v : d; };
        ModelCamera out;
        out.name = c->name ? c->name : std::format("camera{}", c - data_->cameras);
        if (c->type == cgltf_camera_type_orthographic) {
            const auto& o = c->data.orthographic;
            out.orthographic = true;
            out.ymag = fin(o.ymag, 1.0f) * settings_.scale;
            out.aspect = fin(o.xmag, 0.0f) > 0.0f && fin(o.ymag, 0.0f) > 0.0f ? o.xmag / o.ymag : 0.0f;
            out.znear = std::max(std::isfinite(o.znear) ? o.znear : 0.0f, 0.0f) * settings_.scale;
            out.zfar = fin(o.zfar, 100.0f) * settings_.scale;
        } else {
            const auto& p = c->data.perspective;
            out.yfov = std::clamp(fin(p.yfov, 0.8f), 0.01f, 3.1f);
            out.aspect = p.has_aspect_ratio ? fin(p.aspect_ratio, 0.0f) : 0.0f;
            out.znear = fin(p.znear, 0.1f) * settings_.scale;
            out.zfar = p.has_zfar ? fin(p.zfar, 0.0f) * settings_.scale : 0.0f;
            if (out.zfar > 0.0f && out.zfar <= out.znear) out.zfar = 0.0f;
        }
        const auto index = static_cast<std::int32_t>(model_.cameras.size());
        model_.cameras.push_back(std::move(out));
        camera_map_[c] = index;
        return index;
    }

    void import_skins() {
        std::vector<std::int32_t> remap(data_->skins_count, -1);
        for (cgltf_size si = 0; si < data_->skins_count; ++si) {
            const cgltf_skin& src = data_->skins[si];
            ModelSkin skin;
            skin.name = src.name ? src.name : std::format("skin{}", si);
            bool ok = src.joints_count > 0 && src.joints_count <= 65535;
            for (cgltf_size j = 0; ok && j < src.joints_count; ++j) {
                const auto it = node_map_.find(src.joints[j]);
                if (it == node_map_.end()) ok = false;
                else skin.joints.push_back(it->second);
            }
            const cgltf_accessor* ibm = src.inverse_bind_matrices;
            if (ok && ibm && (ibm->type != cgltf_type_mat4 || ibm->count < src.joints_count)) ok = false;
            if (!ok) {
                warn(skin.name + ": joints are not part of the imported scene or inverse bind matrices are invalid; skin dropped");
                continue;
            }
            skin.inverse_bind.resize(skin.joints.size());
            if (ibm) {
                for (cgltf_size j = 0; j < skin.joints.size(); ++j)
                    cgltf_accessor_read_float(ibm, j, &skin.inverse_bind[j].c[0].x, 16);
            }
            remap[si] = static_cast<std::int32_t>(model_.skins.size());
            model_.skins.push_back(std::move(skin));
        }
        for (ModelNode& n : model_.nodes) {
            if (n.skin < 0) continue;
            n.skin = remap[static_cast<std::size_t>(n.skin)];
            if (n.skin < 0) continue;
            // Joint indices beyond the skin would read past the palette: drop the skin instead.
            const std::size_t joints = model_.skins[static_cast<std::size_t>(n.skin)].joints.size();
            for (const std::uint32_t mi : n.meshes)
                for (const render::SkinVertex& v : model_.meshes[mi].data.skin)
                    for (const std::uint16_t j : v.joints)
                        if (j >= joints) n.skin = -1;
            if (n.skin < 0) warn(n.name + ": vertex joint indices exceed the skin's joint count; rendered unskinned");
        }
    }

    void import_animations() {
        std::set<std::string> names;
        for (cgltf_size ai = 0; ai < data_->animations_count; ++ai) {
            const cgltf_animation& src = data_->animations[ai];
            animation::Clip clip;
            clip.name = src.name && *src.name ? src.name : std::format("animation{}", ai);
            while (!names.insert(clip.name).second) clip.name += "_";
            bool warned_weights = false;
            for (cgltf_size ci = 0; ci < src.channels_count; ++ci) {
                const cgltf_animation_channel& ch = src.channels[ci];
                if (!ch.target_node || !ch.sampler || !ch.sampler->input || !ch.sampler->output) continue;
                const auto it = node_map_.find(ch.target_node);
                if (it == node_map_.end()) continue;
                animation::Channel out;
                out.node = it->second;
                switch (ch.target_path) {
                case cgltf_animation_path_type_translation: out.path = animation::Path::Translation; break;
                case cgltf_animation_path_type_rotation: out.path = animation::Path::Rotation; break;
                case cgltf_animation_path_type_scale: out.path = animation::Path::Scale; break;
                case cgltf_animation_path_type_weights: out.path = animation::Path::Weights; break;
                default:
                    if (!warned_weights) warn(clip.name + ": unsupported animation channel path; skipped");
                    warned_weights = true;
                    continue;
                }
                switch (ch.sampler->interpolation) {
                case cgltf_interpolation_type_step: out.interpolation = animation::Interpolation::Step; break;
                case cgltf_interpolation_type_cubic_spline: out.interpolation = animation::Interpolation::CubicSpline; break;
                default: out.interpolation = animation::Interpolation::Linear; break;
                }
                out.times = unpack(ch.sampler->input);
                out.values = unpack(ch.sampler->output);
                if (out.path == animation::Path::Weights && !out.times.empty()) {
                    // One scalar per morph target per key (x3 for cubic tangents).
                    const std::size_t per = out.times.size() * (out.interpolation == animation::Interpolation::CubicSpline ? 3 : 1);
                    const cgltf_mesh* m = ch.target_node->mesh;
                    const std::size_t targets = m && m->primitives_count ? m->primitives[0].targets_count : 0;
                    if (targets == 0 || ch.sampler->output->type != cgltf_type_scalar || out.values.size() != per * targets) {
                        warn(std::format("{}: channel {} morph weights do not match the node's targets; skipped", clip.name, ci));
                        continue;
                    }
                    out.width = static_cast<std::uint32_t>(targets);
                }
                if (out.times.empty() || (out.path != animation::Path::Weights &&
                                          cgltf_num_components(ch.sampler->output->type) != out.components())) {
                    warn(std::format("{}: channel {} has invalid sampler data; skipped", clip.name, ci));
                    continue;
                }
                const ModelNode& node = model_.nodes[out.node];
                if (node.parent < 0 && (out.path == animation::Path::Translation || out.path == animation::Path::Scale) &&
                    settings_.scale != 1.0f)
                    for (float& v : out.values) v *= settings_.scale; // root nodes carry the import scale
                if (out.path == animation::Path::Rotation && out.interpolation != animation::Interpolation::CubicSpline) {
                    for (std::size_t k = 0; k + 3 < out.values.size(); k += 4) {
                        const math::Quat q = math::normalize(math::Quat{out.values[k], out.values[k + 1], out.values[k + 2], out.values[k + 3]});
                        out.values[k] = q.x, out.values[k + 1] = q.y, out.values[k + 2] = q.z, out.values[k + 3] = q.w;
                    }
                }
                if (auto st = animation::validate(animation::Clip{.name = clip.name, .duration = 0, .channels = {out}}, model_.nodes.size());
                    !st) {
                    warn(std::format("{}: channel {}: {}; skipped", clip.name, ci, st.error().message));
                    continue;
                }
                clip.duration = std::max(clip.duration, out.times.back());
                if (settings_.keyframe_reduction)
                    (void)animation::reduce_keyframes(out, {.rotation_degrees = settings_.keyframe_rotation_error,
                                                            .relative = settings_.keyframe_error});
                clip.channels.push_back(std::move(out));
            }
            if (clip.channels.empty()) {
                warn(clip.name + ": no supported channels; animation skipped");
                continue;
            }
            model_.animations.push_back(std::move(clip));
        }
    }

    const fs::Path& path_;
    const ModelImportSettings& settings_;
    cgltf_options& options_;
    ReadContext& ctx_;
    ImportResult& result_;
    cgltf_data* data_{nullptr};
    CookedModel model_;
    std::vector<std::vector<std::uint32_t>> mesh_map_;
    std::map<std::tuple<const cgltf_image*, bool, float>, std::int32_t> textures_;
    std::map<std::pair<const cgltf_image*, const cgltf_image*>, std::int32_t> orm_textures_; // (occlusion, metallic-roughness)
    std::map<const cgltf_material*, std::uint32_t> materials_;
    std::map<const cgltf_node*, std::uint32_t> node_map_;
    std::map<const cgltf_light*, std::int32_t> light_map_;
    std::map<const cgltf_camera*, std::int32_t> camera_map_;
    std::int32_t default_material_{-1};
};

} // namespace

namespace {

Result<CookedModel> import_gltf_bytes(std::span<const std::uint8_t> bytes, const fs::Path& path,
                                      const ModelImportSettings& settings, ReadContext& ctx, ImportResult& res) {
    cgltf_options options{};
    options.file.read = sandboxed_read;
    options.file.release = sandboxed_release;
    options.file.user_data = &ctx;
    DataGuard guard;
    cgltf_result r = cgltf_parse(&options, bytes.data(), bytes.size(), &guard.data);
    if (r != cgltf_result_success)
        return make_error(ErrorCode::ParseError, std::format("{}: {}", path.filename().string(), result_name(r)));
    const std::string path_s = path.string();
    r = cgltf_load_buffers(&options, guard.data, path_s.c_str());
    if (r != cgltf_result_success)
        return make_error(ErrorCode::ParseError, std::format("{}: loading buffers failed: {} {}", path.filename().string(),
                                                             result_name(r), ctx.error));
    if (auto checked = validate_accessors(*guard.data); !checked)
        return make_error(ErrorCode::ParseError, std::format("{}: validation failed: {}", path.filename().string(),
                                                             checked.error().message));
    r = cgltf_validate(guard.data);
    if (r != cgltf_result_success)
        return make_error(ErrorCode::ParseError, std::format("{}: validation failed: {}", path.filename().string(), result_name(r)));
    for (cgltf_size i = 0; i < guard.data->extensions_required_count; ++i) {
        const std::string_view ext = guard.data->extensions_required[i];
        // KHR_mesh_quantization: every attribute and sparse morph delta is decoded through
        // cgltf_accessor_read_float / unpack_floats, which dequantise normalised integers.
        if (ext != "KHR_materials_emissive_strength" && ext != "KHR_materials_unlit" && ext != "KHR_mesh_quantization")
            res.warnings.push_back(std::format("required extension {} is not supported", ext));
    }
    GltfImporter importer(path, settings, options, ctx, res);
    return importer.run(guard.data);
}

} // namespace

Result<CookedModel> import_gltf(const fs::Path& path, const ModelImportSettings& settings, const fs::Path& sandbox_root,
                                ImportResult* result) {
    ImportResult local;
    ImportResult& res = result ? *result : local;
    std::vector<fs::Path> deps;
    ReadContext ctx{sandbox_root, &deps, {}};
    auto bytes = fs::read_binary(path, 1ull << 31);
    if (!bytes) return std::move(bytes).error();
    auto model = import_gltf_bytes(*bytes, path, settings, ctx, res);
    if (model)
        if (auto m = apply_import_skeleton_migrations(*model, settings, path, sandbox_root, deps); !m) return std::move(m).error();
    std::sort(deps.begin(), deps.end());
    deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
    res.dependencies = std::move(deps);
    return model;
}

Result<CookedModel> import_gltf_memory(std::span<const std::uint8_t> bytes, std::string_view name,
                                       const ModelImportSettings& settings, ImportResult* result) {
    ImportResult local;
    ImportResult& res = result ? *result : local;
    ReadContext ctx{{}, nullptr, {}, true};
    if (!settings.skeleton_migrations.empty()) res.warnings.push_back("skeleton migrations need a file import; skipped");
    return import_gltf_bytes(bytes, utf8_path(name), settings, ctx, res);
}

} // namespace victus::assets
