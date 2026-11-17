// SPDX-License-Identifier: Apache-2.0
#include "victus/assets/importers.hpp"

#include "cgltf.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
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
    return v;
}

TextureImportSettings TextureImportSettings::from_json(const json::Value& v) {
    TextureImportSettings s;
    s.srgb = v["srgb"].as_bool(s.srgb);
    s.mipmaps = v["mipmaps"].as_bool(s.mipmaps);
    s.max_size = static_cast<std::uint32_t>(std::clamp<std::int64_t>(v["max_size"].as_int(s.max_size), 1, 16384));
    s.flip_y = v["flip_y"].as_bool(s.flip_y);
    s.nearest = v["filter"].as_string() == "nearest";
    return s;
}

json::Value ModelImportSettings::to_json() const {
    json::Value v = json::Value::object();
    v.set("scale", scale);
    v.set("generate_tangents", generate_tangents);
    v.set("import_textures", import_textures);
    v.set("max_texture_size", max_texture_size);
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
    return s;
}

// ---- textures ----------------------------------------------------------------------------------

CookedTexture cook_texture(const render::Image& image, std::string name, bool srgb, bool mipmaps, std::uint32_t max_size) {
    CookedTexture t;
    t.name = std::move(name);
    t.srgb = srgb;
    const bool oversized = image.width > max_size || image.height > max_size;
    if (mipmaps || oversized) {
        t.mips = render::build_mip_chain(image, srgb);
        std::size_t drop = 0;
        while (drop + 1 < t.mips.size() && (t.mips[drop].width > max_size || t.mips[drop].height > max_size)) ++drop;
        t.mips.erase(t.mips.begin(), t.mips.begin() + static_cast<std::ptrdiff_t>(drop));
        if (!mipmaps) t.mips.resize(1);
    } else {
        t.mips.push_back(image);
    }
    return t;
}

Result<CookedTexture> import_texture(const fs::Path& path, const TextureImportSettings& settings) {
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
    return cook_texture(image, path.filename().string(), settings.srgb, settings.mipmaps, settings.max_size);
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
        if (model_.meshes.empty()) return make_error(ErrorCode::InvalidArgument, "glTF contains no triangle meshes");
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

    std::int32_t texture(const cgltf_texture_view& view, bool srgb, const char* slot) {
        if (!view.texture || !settings_.import_textures) return -1;
        const cgltf_image* image = view.texture->image;
        if (!image) {
            warn(std::format("{} texture uses an unsupported image extension (e.g. KTX2/WebP); skipped", slot));
            return -1;
        }
        if (view.texcoord != 0) warn(std::format("{} texture uses TEXCOORD_{}; only TEXCOORD_0 is supported", slot, view.texcoord));
        const auto key = std::make_pair(image, srgb);
        if (const auto it = textures_.find(key); it != textures_.end()) return it->second;
        auto img = load_image(*image);
        std::int32_t index = -1;
        if (!img) {
            warn(std::format("{} texture '{}': {}", slot, image->name ? image->name : (image->uri ? image->uri : "?"),
                             img.error().message));
        } else {
            std::string name = image->name ? image->name : std::format("image{}", image - data_->images);
            index = static_cast<std::int32_t>(model_.textures.size());
            model_.textures.push_back(cook_texture(*img, std::move(name), srgb, true, settings_.max_texture_size));
        }
        textures_.emplace(key, index);
        return index;
    }

    std::uint32_t material(const cgltf_material* m) {
        if (!m) return default_material();
        if (const auto it = materials_.find(m); it != materials_.end()) return it->second;
        ModelMaterial out;
        out.name = m->name ? m->name : std::format("material{}", m - data_->materials);
        if (m->has_pbr_metallic_roughness) {
            const auto& pbr = m->pbr_metallic_roughness;
            out.base_color = {pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2], pbr.base_color_factor[3]};
            out.metallic = pbr.metallic_factor;
            out.roughness = pbr.roughness_factor;
            out.base_color_texture = texture(pbr.base_color_texture, true, "baseColor");
            out.metallic_roughness_texture = texture(pbr.metallic_roughness_texture, false, "metallicRoughness");
        } else {
            out.metallic = 0.0f;
            out.roughness = 0.5f;
            if (m->has_pbr_specular_glossiness) warn(out.name + ": KHR_materials_pbrSpecularGlossiness approximated by defaults");
        }
        out.normal_texture = texture(m->normal_texture, false, "normal");
        out.normal_scale = m->normal_texture.texture ? m->normal_texture.scale : 1.0f;
        out.emissive = {m->emissive_factor[0], m->emissive_factor[1], m->emissive_factor[2], 1.0f};
        out.emissive_texture = texture(m->emissive_texture, true, "emissive");
        if (out.emissive_texture >= 0 && out.emissive.r == 0 && out.emissive.g == 0 && out.emissive.b == 0)
            warn(out.name + ": emissive texture with zero emissiveFactor has no effect");
        out.emissive_strength = m->has_emissive_strength ? m->emissive_strength.emissive_strength : 1.0f;
        out.double_sided = m->double_sided;
        out.unlit = m->unlit;
        switch (m->alpha_mode) {
        case cgltf_alpha_mode_mask: out.alpha_cutoff = m->alpha_cutoff; break;
        case cgltf_alpha_mode_blend:
            out.alpha_cutoff = 0.5f;
            warn(out.name + ": alpha blending is rendered as alpha-tested (cutoff 0.5) until the transparency pass lands");
            break;
        default: out.alpha_cutoff = 0.0f; break;
        }
        const auto index = static_cast<std::uint32_t>(model_.materials.size());
        model_.materials.push_back(std::move(out));
        materials_.emplace(m, index);
        return index;
    }

    static std::vector<float> unpack(const cgltf_accessor* acc) {
        const cgltf_size n = acc->count * cgltf_num_components(acc->type);
        std::vector<float> out(n);
        cgltf_accessor_unpack_floats(acc, out.data(), n);
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
        if (node->weights_count > 0 || (node->mesh && node->mesh->weights_count > 0))
            warn(n.name + ": morph target weights are not supported; base shape used");
        const auto index = static_cast<std::int32_t>(model_.nodes.size());
        node_map_[node] = static_cast<std::uint32_t>(index);
        model_.nodes.push_back(std::move(n));
        for (cgltf_size c = 0; c < node->children_count; ++c) add_node(node->children[c], index, depth + 1);
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
                default:
                    if (!warned_weights) warn(clip.name + ": morph weight channels are not supported; skipped");
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
                if (out.times.empty() || cgltf_num_components(ch.sampler->output->type) != out.components()) {
                    warn(std::format("{}: channel {} has invalid sampler data; skipped", clip.name, ci));
                    continue;
                }
                const ModelNode& node = model_.nodes[out.node];
                if (node.parent < 0 && out.path != animation::Path::Rotation && settings_.scale != 1.0f)
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
    std::map<std::pair<const cgltf_image*, bool>, std::int32_t> textures_;
    std::map<const cgltf_material*, std::uint32_t> materials_;
    std::map<const cgltf_node*, std::uint32_t> node_map_;
    std::int32_t default_material_{-1};
};

} // namespace

Result<CookedModel> import_gltf(const fs::Path& path, const ModelImportSettings& settings, const fs::Path& sandbox_root,
                                ImportResult* result) {
    ImportResult local;
    ImportResult& res = result ? *result : local;
    std::vector<fs::Path> deps;
    ReadContext ctx{sandbox_root, &deps, {}};
    cgltf_options options{};
    options.file.read = sandboxed_read;
    options.file.release = sandboxed_release;
    options.file.user_data = &ctx;

    auto bytes = fs::read_binary(path, 1ull << 31);
    if (!bytes) return std::move(bytes).error();
    DataGuard guard;
    cgltf_result r = cgltf_parse(&options, bytes->data(), bytes->size(), &guard.data);
    if (r != cgltf_result_success)
        return make_error(ErrorCode::ParseError, std::format("{}: {}", path.filename().string(), result_name(r)));
    const std::string path_s = path.string();
    r = cgltf_load_buffers(&options, guard.data, path_s.c_str());
    if (r != cgltf_result_success)
        return make_error(ErrorCode::ParseError, std::format("{}: loading buffers failed: {} {}", path.filename().string(),
                                                             result_name(r), ctx.error));
    r = cgltf_validate(guard.data);
    if (r != cgltf_result_success)
        return make_error(ErrorCode::ParseError, std::format("{}: validation failed: {}", path.filename().string(), result_name(r)));
    for (cgltf_size i = 0; i < guard.data->extensions_required_count; ++i) {
        const std::string_view ext = guard.data->extensions_required[i];
        if (ext != "KHR_materials_emissive_strength" && ext != "KHR_materials_unlit")
            res.warnings.push_back(std::format("required extension {} is not supported", ext));
    }
    GltfImporter importer(path, settings, options, ctx, res);
    auto model = importer.run(guard.data);
    std::sort(deps.begin(), deps.end());
    deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
    res.dependencies = std::move(deps);
    return model;
}

} // namespace victus::assets
