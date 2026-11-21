// SPDX-License-Identifier: Apache-2.0
// FBX (binary and ASCII, 6.x/7.x) and Wavefront OBJ import through ufbx (ADR-0173). The scene is
// converted by ufbx to VICTUS space (Y up, -Z forward, metres) with pivots, geometric transforms
// and inherit modes folded into plain TRS nodes, then mapped onto the same CookedModel the glTF
// importer produces: meshes split per material, materials, textures, nodes, skins, morph
// targets, baked animation clips, lights and cameras.
#include "victus/assets/importers.hpp"

#include "ufbx.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace victus::assets {
namespace {

constexpr std::uint64_t kMaxFbxBytes = 1ull << 31;

// Builds a path from UTF-8 bytes without relying on the narrow (ANSI on Windows) code page.
fs::Path utf8_path(std::string_view bytes) {
    std::u8string u;
    u.reserve(bytes.size());
    for (const char c : bytes) u.push_back(static_cast<char8_t>(c));
    return fs::Path(u);
}

bool inside_root(const fs::Path& root, const fs::Path& p) {
    std::error_code ec;
    const fs::Path r = std::filesystem::weakly_canonical(root, ec);
    if (ec) return false;
    const fs::Path c = std::filesystem::weakly_canonical(p, ec);
    if (ec) return false;
    auto ri = r.begin();
    auto ci = c.begin();
    for (; ri != r.end(); ++ri, ++ci)
        if (ci == c.end() || *ri != *ci) return false;
    return true;
}

std::string to_string(ufbx_string s) { return std::string(s.data, s.length); }

struct FileContext {
    fs::Path sandbox;
    std::vector<fs::Path>* dependencies{nullptr};
    std::vector<std::string>* warnings{nullptr};
    bool memory_only{false};
};

// External files (OBJ .mtl) are read only from inside the sandbox; everything else fails.
bool sandboxed_open(void* user, ufbx_stream* stream, const char* path, size_t path_len, const ufbx_open_file_info* info) {
    auto* ctx = static_cast<FileContext*>(user);
    const std::string_view sv(path, path_len);
    if (ctx->memory_only || sv.find("://") != std::string_view::npos) return false;
    const fs::Path p = utf8_path(sv);
    if (!inside_root(ctx->sandbox, p)) {
        if (ctx->warnings) ctx->warnings->push_back(std::format("reference outside the asset directory ignored: {}", sv));
        return false;
    }
    auto bytes = fs::read_binary(p, kMaxFbxBytes);
    if (!bytes) return false;
    if (ctx->dependencies) ctx->dependencies->push_back(p);
    ufbx_open_memory_opts mo{};
    // ufbx copies the data (no_copy is false), so the vector may go away.
    return ufbx_open_memory_ctx(stream, info->context, bytes->data(), bytes->size(), &mo, nullptr);
}

math::Vec3 vec3(ufbx_vec3 v) { return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)}; }
math::Quat quat(ufbx_quat q) {
    return math::normalize(math::Quat{static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z), static_cast<float>(q.w)});
}
math::Mat4 mat4(const ufbx_matrix& m) {
    math::Mat4 r;
    for (int c = 0; c < 4; ++c)
        r.c[c] = {static_cast<float>(m.cols[c].x), static_cast<float>(m.cols[c].y), static_cast<float>(m.cols[c].z), c == 3 ? 1.0f : 0.0f};
    return r;
}
bool finite3(math::Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// One vertex as gathered from the FBX before de-duplication; `source` (the FBX control point)
// is part of the key so morph targets and skin weights stay attached to it.
struct RawVertex {
    render::Vertex3D v;
    std::uint32_t source{0};
    std::uint32_t pad{0};
};
static_assert(sizeof(RawVertex) == 60);

class FbxImporter {
public:
    FbxImporter(const ufbx_scene* scene, fs::Path path, const ModelImportSettings& settings, FileContext& files, ImportResult& result)
        : scene_(scene), path_(std::move(path)), settings_(settings), files_(files), result_(result) {}

    Result<CookedModel> run() {
        for (std::size_t i = 0; i < scene_->root_node->children.count; ++i) add_node(scene_->root_node->children.data[i], -1, 0);
        for (auto& n : model_.nodes)
            if (n.parent < 0) {
                n.translation = n.translation * settings_.scale;
                n.scale = n.scale * settings_.scale;
            }
        if (auto st = import_meshes(); !st) return std::move(st).error();
        if (model_.materials.empty() && !model_.meshes.empty()) default_material();
        import_animations();
        if (model_.meshes.empty() && model_.lights.empty() && model_.cameras.empty())
            return make_error(ErrorCode::InvalidArgument, std::format("{}: no triangle meshes, lights or cameras", path_.filename().string()));
        if (auto st = validate_model(model_); !st)
            return make_error(ErrorCode::ParseError, std::format("{}: {}", path_.filename().string(), st.error().message));
        return std::move(model_);
    }

private:
    void warn(std::string w) { result_.warnings.push_back(std::move(w)); }

    void add_node(const ufbx_node* node, std::int32_t parent, int depth) {
        if (depth > 256) {
            warn("node hierarchy deeper than 256 levels truncated");
            return;
        }
        ModelNode n;
        n.name = node->name.length ? to_string(node->name) : std::format("node{}", node->typed_id);
        n.parent = parent;
        n.translation = vec3(node->local_transform.translation);
        n.rotation = quat(node->local_transform.rotation);
        n.scale = vec3(node->local_transform.scale);
        if (!finite3(n.translation) || !finite3(n.scale)) {
            warn(n.name + ": non-finite transform reset");
            n.translation = {};
            n.scale = {1, 1, 1};
        }
        if (node->light && settings_.import_lights) n.light = light(node->light, n.name);
        if (node->camera && settings_.import_cameras) n.camera = camera(node->camera, n.name);
        const auto index = static_cast<std::int32_t>(model_.nodes.size());
        node_map_[node] = static_cast<std::uint32_t>(index);
        model_.nodes.push_back(std::move(n));
        if (node->mesh) mesh_nodes_.emplace_back(node, static_cast<std::uint32_t>(index));
        for (std::size_t c = 0; c < node->children.count; ++c) add_node(node->children.data[c], index, depth + 1);
    }

    // FBX light intensities are the DCC's own numbers (ufbx divides the stored percentage by
    // 100), not photometric, so light_intensity_scale (a glTF unit conversion) is not applied.
    std::int32_t light(const ufbx_light* l, const std::string& node_name) {
        if (auto it = light_map_.find(l); it != light_map_.end()) return it->second;
        ModelLight out;
        out.name = l->name.length ? to_string(l->name) : node_name;
        switch (l->type) {
        case UFBX_LIGHT_DIRECTIONAL: out.type = ModelLight::Type::Directional; break;
        case UFBX_LIGHT_SPOT: out.type = ModelLight::Type::Spot; break;
        case UFBX_LIGHT_POINT: out.type = ModelLight::Type::Point; break;
        default:
            out.type = ModelLight::Type::Point;
            warn(out.name + ": area/volume light imported as a point light");
            break;
        }
        const math::Vec3 c = vec3(l->color);
        out.color = {std::max(std::isfinite(c.x) ? c.x : 1.0f, 0.0f), std::max(std::isfinite(c.y) ? c.y : 1.0f, 0.0f),
                     std::max(std::isfinite(c.z) ? c.z : 1.0f, 0.0f), 1.0f};
        out.intensity = std::isfinite(l->intensity) ? std::max(static_cast<float>(l->intensity), 0.0f) : 1.0f;
        if (out.type == ModelLight::Type::Spot) {
            // FBX cone angles are full angles in degrees.
            constexpr float kHalfDegToRad = 0.5f * 0.017453293f;
            const float outer = std::isfinite(l->outer_angle) && l->outer_angle > 0 ? static_cast<float>(l->outer_angle) : 45.0f;
            const float inner = std::isfinite(l->inner_angle) ? static_cast<float>(l->inner_angle) : 0.0f;
            out.outer_cone = std::clamp(outer * kHalfDegToRad, 0.001f, 1.5707963f);
            out.inner_cone = std::clamp(inner * kHalfDegToRad, 0.0f, out.outer_cone);
        }
        const math::Vec3 dir = vec3(l->local_direction);
        if (out.type != ModelLight::Type::Point && (std::abs(dir.z + 1.0f) > 1e-3f))
            warn(std::format("{}: light aims along ({:.2f}, {:.2f}, {:.2f}) in its node, not -Z", out.name, dir.x, dir.y, dir.z));
        const auto index = static_cast<std::int32_t>(model_.lights.size());
        model_.lights.push_back(std::move(out));
        light_map_[l] = index;
        return index;
    }

    std::int32_t camera(const ufbx_camera* c, const std::string& node_name) {
        if (auto it = camera_map_.find(c); it != camera_map_.end()) return it->second;
        ModelCamera out;
        out.name = c->name.length ? to_string(c->name) : node_name;
        const auto pos = [](double v, float d) { return std::isfinite(v) && v > 0.0 ? static_cast<float>(v) : d; };
        out.aspect = pos(c->aspect_ratio, 0.0f);
        out.znear = pos(c->near_plane, 0.1f) * settings_.scale;
        out.zfar = pos(c->far_plane, 0.0f) * settings_.scale;
        if (out.zfar <= out.znear) out.zfar = 0.0f;
        if (c->projection_mode == UFBX_PROJECTION_MODE_ORTHOGRAPHIC) {
            out.orthographic = true;
            out.ymag = pos(c->orthographic_size.y, 2.0f) * 0.5f * settings_.scale;
        } else {
            out.yfov = std::clamp(pos(c->field_of_view_deg.y, 45.0f) * 0.017453293f, 0.01f, 3.1f);
        }
        const auto index = static_cast<std::int32_t>(model_.cameras.size());
        model_.cameras.push_back(std::move(out));
        camera_map_[c] = index;
        return index;
    }

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

    // Embedded content first, then the relative path next to the model, then the bare file name
    // next to the model or in a "textures" folder (absolute paths from the authoring machine).
    Result<render::Image> load_image(const ufbx_texture& t) {
        if (t.content.size > 0) {
            std::vector<std::uint8_t> bytes(static_cast<const std::uint8_t*>(t.content.data),
                                            static_cast<const std::uint8_t*>(t.content.data) + t.content.size);
            return render::decode_image(bytes);
        }
        if (files_.memory_only) return make_error(ErrorCode::NotFound, "external texture in an in-memory model");
        const fs::Path dir = path_.parent_path();
        std::vector<fs::Path> candidates;
        if (t.relative_filename.length) candidates.push_back(dir / utf8_path(to_string(t.relative_filename)));
        std::string name = to_string(t.filename.length ? t.filename : t.relative_filename);
        std::replace(name.begin(), name.end(), '\\', '/');
        const fs::Path base = utf8_path(name).filename();
        if (!base.empty()) {
            candidates.push_back(dir / base);
            candidates.push_back(dir / "textures" / base);
            candidates.push_back(dir / "Textures" / base);
        }
        for (const fs::Path& p : candidates) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(p, ec) || !inside_root(files_.sandbox, p)) continue;
            auto bytes = fs::read_binary(p, kMaxFbxBytes);
            if (!bytes) continue;
            if (files_.dependencies) files_.dependencies->push_back(p);
            return render::decode_image(*bytes);
        }
        return make_error(ErrorCode::NotFound, std::format("'{}' not found next to the model", name));
    }

    std::int32_t texture(const ufbx_texture* t, bool srgb, const char* slot, float alpha_cutoff = 0.0f) {
        if (!t || !settings_.import_textures) return -1;
        if (t->type == UFBX_TEXTURE_LAYERED && t->layers.count) t = t->layers.data[0].texture;
        if (t->type != UFBX_TEXTURE_FILE) {
            warn(std::format("{} texture '{}' is procedural or layered; skipped", slot, to_string(t->name)));
            return -1;
        }
        const auto key = std::make_tuple(t, srgb, alpha_cutoff);
        if (const auto it = textures_.find(key); it != textures_.end()) return it->second;
        std::int32_t index = -1;
        auto img = load_image(*t);
        if (!img) {
            warn(std::format("{} texture '{}': {}", slot, to_string(t->name), img.error().message));
        } else {
            std::string name = t->name.length ? to_string(t->name) : std::format("texture{}", t->typed_id);
            CookedTexture ct = cook_texture(*img, std::move(name), srgb, true, settings_.max_texture_size, alpha_cutoff);
            if (auto r = compress_cooked_texture(ct, settings_.texture_compression); !r)
                warn(std::format("{} texture '{}': {}", slot, ct.name, r.error().message));
            index = static_cast<std::int32_t>(model_.textures.size());
            model_.textures.push_back(std::move(ct));
        }
        textures_.emplace(key, index);
        return index;
    }

    static const ufbx_texture* map_texture(const ufbx_material_map& m) { return m.texture_enabled || m.texture ? m.texture : nullptr; }

    std::uint32_t material(const ufbx_material* m) {
        if (!m) return default_material();
        if (const auto it = materials_.find(m); it != materials_.end()) return it->second;
        ModelMaterial out;
        out.name = m->name.length ? to_string(m->name) : std::format("material{}", m->typed_id);
        const auto& pbr = m->pbr;
        const auto real = [](const ufbx_material_map& map, float d) {
            return map.has_value && std::isfinite(map.value_real) ? static_cast<float>(map.value_real) : d;
        };
        const float base_factor = real(pbr.base_factor, 1.0f);
        const ufbx_vec4 bc = pbr.base_color.value_vec4;
        if (pbr.base_color.has_value)
            out.base_color = {static_cast<float>(bc.x) * base_factor, static_cast<float>(bc.y) * base_factor,
                              static_cast<float>(bc.z) * base_factor, 1.0f};
        const float opacity = std::clamp(real(pbr.opacity, 1.0f), 0.0f, 1.0f);
        const ufbx_texture* opacity_tex = map_texture(pbr.opacity);
        if (opacity < 1.0f || opacity_tex) {
            out.alpha_cutoff = 0.5f;
            out.base_color.a = opacity;
            warn(out.name + ": transparency is rendered as alpha-tested (cutoff 0.5)");
        }
        out.metallic = std::clamp(real(pbr.metalness, 0.0f), 0.0f, 1.0f);
        float roughness = real(pbr.roughness, 0.5f);
        if (m->features.roughness_as_glossiness.enabled) roughness = 1.0f - roughness;
        out.roughness = std::clamp(roughness, 0.0f, 1.0f);
        out.base_color_texture = texture(map_texture(pbr.base_color), true, "baseColor", out.alpha_cutoff);
        // VICTUS samples roughness (G) and metallic (B) from one texture: use a shared one.
        const ufbx_texture* rough = map_texture(pbr.roughness);
        const ufbx_texture* metal = map_texture(pbr.metalness);
        if (rough && (rough == metal || !metal)) out.metallic_roughness_texture = texture(rough, false, "metallicRoughness");
        else if (rough || metal) warn(out.name + ": separate roughness/metalness textures are not packed yet; factors used");
        if (out.metallic_roughness_texture >= 0) {
            out.metallic = metal ? 1.0f : out.metallic;
            out.roughness = 1.0f;
        }
        out.normal_texture = texture(map_texture(pbr.normal_map), false, "normal");
        if (const ufbx_texture* bump = map_texture(m->fbx.bump); out.normal_texture < 0 && bump)
            warn(out.name + ": bump (height) maps are not converted to normal maps");
        const float emission_factor = real(pbr.emission_factor, 1.0f);
        if (pbr.emission_color.has_value) {
            const ufbx_vec4 e = pbr.emission_color.value_vec4;
            out.emissive = {static_cast<float>(e.x) * emission_factor, static_cast<float>(e.y) * emission_factor,
                            static_cast<float>(e.z) * emission_factor, 1.0f};
        }
        out.emissive_texture = texture(map_texture(pbr.emission_color), true, "emissive");
        if (out.emissive_texture >= 0 && out.emissive.r == 0 && out.emissive.g == 0 && out.emissive.b == 0) out.emissive = {1, 1, 1, 1};
        out.double_sided = m->features.double_sided.enabled;
        out.unlit = m->features.unlit.enabled;
        const auto index = static_cast<std::uint32_t>(model_.materials.size());
        model_.materials.push_back(std::move(out));
        materials_.emplace(m, index);
        return index;
    }

    std::int32_t skin(const ufbx_skin_deformer* s) {
        if (const auto it = skin_map_.find(s); it != skin_map_.end()) return it->second;
        ModelSkin out;
        out.name = s->name.length ? to_string(s->name) : std::format("skin{}", s->typed_id);
        for (std::size_t ci = 0; ci < s->clusters.count; ++ci) {
            const ufbx_skin_cluster* cl = s->clusters.data[ci];
            const auto it = cl->bone_node ? node_map_.find(cl->bone_node) : node_map_.end();
            if (it == node_map_.end()) {
                skin_map_[s] = -1;
                warn(out.name + ": a bone is outside the scene; skin ignored");
                return -1;
            }
            out.joints.push_back(it->second);
            // Root nodes carry the import scale, which the joint world matrices then include.
            const math::Mat4 ib = mat4(cl->geometry_to_bone);
            out.inverse_bind.push_back(ib);
        }
        if (out.joints.empty() || out.joints.size() > 65535) {
            skin_map_[s] = -1;
            return -1;
        }
        const auto index = static_cast<std::int32_t>(model_.skins.size());
        model_.skins.push_back(std::move(out));
        skin_map_[s] = index;
        return index;
    }

    // Maya/Max keep vertices without any weight rigidly on the mesh node; VICTUS would bind them
    // to joint 0, so the mesh node itself becomes an extra joint (identity inverse bind).
    void rigid_joint(const ufbx_skin_deformer& sk, std::uint32_t mesh_node) {
        if (rigid_joint_.contains(&sk)) return;
        bool unweighted = false;
        for (std::size_t vi = 0; vi < sk.vertices.count && !unweighted; ++vi) {
            const ufbx_skin_vertex& sv = sk.vertices.data[vi];
            double total = 0.0;
            for (std::size_t wi = 0; wi < sv.num_weights; ++wi) total += std::max(sk.weights.data[sv.weight_begin + wi].weight, 0.0);
            unweighted = !(total > 0.0);
        }
        std::int32_t joint = -1;
        ModelSkin& skin = model_.skins[static_cast<std::size_t>(skin_map_[&sk])];
        if (unweighted && skin.joints.size() < 65535) {
            joint = static_cast<std::int32_t>(skin.joints.size());
            skin.joints.push_back(mesh_node);
            skin.inverse_bind.push_back(math::Mat4{});
        }
        rigid_joint_[&sk] = joint;
    }

    Status import_meshes() {
        // ModelMeshes per (ufbx mesh, material list) so instances share their geometry.
        std::map<std::pair<const ufbx_mesh*, std::vector<const ufbx_material*>>, std::vector<std::uint32_t>> built;
        for (const auto& [node, node_index] : mesh_nodes_) {
            const ufbx_mesh* mesh = node->mesh;
            std::vector<const ufbx_material*> mats;
            const ufbx_material_list& list = node->materials.count ? node->materials : mesh->materials;
            for (std::size_t i = 0; i < list.count; ++i) mats.push_back(list.data[i]);
            ModelNode& mn = model_.nodes[node_index];
            const ufbx_skin_deformer* sk = mesh->skin_deformers.count ? mesh->skin_deformers.data[0] : nullptr;
            if (sk) {
                if (mesh->skin_deformers.count > 1) warn(mn.name + ": only the first skin deformer is used");
                mn.skin = skin(sk);
                if (mn.skin >= 0) rigid_joint(*sk, node_index);
            }
            auto key = std::make_pair(mesh, mats);
            auto it = built.find(key);
            if (it == built.end()) {
                std::vector<std::uint32_t> parts;
                if (auto st = build_mesh(*mesh, mats, mn.skin >= 0 ? sk : nullptr, mn.name, parts); !st) return st;
                it = built.emplace(std::move(key), std::move(parts)).first;
            }
            mn.meshes = it->second;
            if (mn.meshes.empty()) continue;
            if (!model_.meshes[mn.meshes[0]].data.morphs.empty()) morph_nodes_.emplace_back(mesh, node_index);
        }
        return {};
    }

    Status build_mesh(const ufbx_mesh& mesh, const std::vector<const ufbx_material*>& mats, const ufbx_skin_deformer* sk,
                      const std::string& node_name, std::vector<std::uint32_t>& parts) {
        if (mesh.num_triangles == 0) return {};
        if (mesh.num_triangles > (1u << 28)) return make_error(ErrorCode::LimitExceeded, node_name + ": mesh too large");
        const std::string mesh_name = mesh.name.length ? to_string(mesh.name) : node_name;
        // Per control point skin influences.
        std::vector<render::SkinVertex> point_skin;
        if (sk) {
            point_skin.resize(mesh.num_vertices);
            for (std::size_t vi = 0; vi < mesh.num_vertices && vi < sk->vertices.count; ++vi) {
                const ufbx_skin_vertex& sv = sk->vertices.data[vi];
                std::uint32_t j[8]{};
                float w[8]{};
                std::size_t n = 0;
                const std::int32_t rigid = rigid_joint_[sk];
                for (std::size_t wi = 0; wi < sv.num_weights && n < 8; ++wi) {
                    const ufbx_skin_weight& sw = sk->weights.data[sv.weight_begin + wi];
                    if (sw.cluster_index > 65535 || !std::isfinite(sw.weight)) continue;
                    j[n] = sw.cluster_index;
                    w[n++] = std::max(static_cast<float>(sw.weight), 0.0f);
                }
                float total = 0.0f;
                for (std::size_t k = 0; k < n; ++k) total += w[k];
                if (!(total > 0.0f) && rigid >= 0) {
                    j[0] = static_cast<std::uint32_t>(rigid);
                    w[0] = 1.0f;
                    n = 1;
                }
                point_skin[vi] = render::make_skin_vertex(j, w, n);
            }
        }
        // Blend channels (morph targets), indexed by control point.
        std::vector<const ufbx_blend_channel*> channels;
        for (std::size_t d = 0; d < mesh.blend_deformers.count; ++d)
            for (std::size_t c = 0; c < mesh.blend_deformers.data[d]->channels.count; ++c)
                channels.push_back(mesh.blend_deformers.data[d]->channels.data[c]);
        if (channels.size() > render::kMaxMorphTargets) {
            warn(mesh_name + ": more than 256 blend shapes; extra ones dropped");
            channels.resize(render::kMaxMorphTargets);
        }
        const bool flip_v = true; // FBX/OBJ uv origin is bottom-left, VICTUS (glTF) top-left
        std::vector<std::uint32_t> tri(mesh.max_face_triangles * 3);
        for (std::size_t pi = 0; pi < mesh.material_parts.count; ++pi) {
            const ufbx_mesh_part& part = mesh.material_parts.data[pi];
            if (part.num_triangles == 0) continue;
            std::vector<RawVertex> raw;
            raw.reserve(part.num_triangles * 3);
            for (std::size_t fi = 0; fi < part.num_faces; ++fi) {
                const ufbx_face face = mesh.faces.data[part.face_indices.data[fi]];
                const std::uint32_t n = ufbx_triangulate_face(tri.data(), tri.size(), &mesh, face);
                for (std::uint32_t k = 0; k < n * 3; ++k) {
                    const std::uint32_t ix = tri[k];
                    RawVertex rv{};
                    rv.v.position = vec3(ufbx_get_vertex_vec3(&mesh.vertex_position, ix));
                    if (mesh.vertex_normal.exists) rv.v.normal = math::normalize(vec3(ufbx_get_vertex_vec3(&mesh.vertex_normal, ix)));
                    if (!finite3(rv.v.normal)) rv.v.normal = {0, 1, 0};
                    if (mesh.vertex_uv.exists) {
                        const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh.vertex_uv, ix);
                        rv.v.uv = {static_cast<float>(uv.x), flip_v ? 1.0f - static_cast<float>(uv.y) : static_cast<float>(uv.y)};
                    }
                    rv.v.tangent = {1, 0, 0, 1};
                    if (mesh.vertex_color.exists) {
                        // DCC vertex colours are authored as display (sRGB) values.
                        const ufbx_vec4 c = ufbx_get_vertex_vec4(&mesh.vertex_color, ix);
                        const auto b = [](double x) { return static_cast<std::uint8_t>(std::lround(std::clamp(std::isfinite(x) ? x : 1.0, 0.0, 1.0) * 255.0)); };
                        rv.v.color = render::pack_vertex_color_srgb8(b(c.x), b(c.y), b(c.z), b(c.w));
                    }
                    rv.source = mesh.vertex_indices.data[ix];
                    if (!finite3(rv.v.position)) return make_error(ErrorCode::ParseError, mesh_name + ": non-finite vertex position");
                    raw.push_back(rv);
                }
            }
            std::vector<std::uint32_t> indices(raw.size());
            ufbx_vertex_stream stream{raw.data(), raw.size(), sizeof(RawVertex)};
            ufbx_error err{};
            const std::size_t count = ufbx_generate_indices(&stream, 1, indices.data(), indices.size(), nullptr, &err);
            if (err.type != UFBX_ERROR_NONE) return make_error(ErrorCode::ParseError, mesh_name + ": " + to_string(err.description));
            ModelMesh out;
            out.name = mesh.material_parts.count > 1 ? std::format("{}[{}]", mesh_name, pi) : mesh_name;
            out.data.vertices.resize(count);
            for (std::size_t i = 0; i < count; ++i) out.data.vertices[i] = raw[i].v;
            out.data.indices = std::move(indices);
            if (sk) {
                out.data.skin.resize(count);
                for (std::size_t i = 0; i < count; ++i)
                    out.data.skin[i] = raw[i].source < point_skin.size() ? point_skin[raw[i].source] : render::SkinVertex{};
            }
            if (!channels.empty()) {
                // Control point -> output vertices, for the sparse offsets.
                std::unordered_multimap<std::uint32_t, std::uint32_t> by_source;
                by_source.reserve(count);
                for (std::size_t i = 0; i < count; ++i) by_source.emplace(raw[i].source, static_cast<std::uint32_t>(i));
                for (const ufbx_blend_channel* ch : channels) {
                    render::MorphTarget target;
                    target.name = ch->name.length ? to_string(ch->name) : std::format("shape{}", ch->typed_id);
                    const ufbx_blend_shape* shape = ch->target_shape;
                    if (shape) {
                        std::vector<std::pair<std::uint32_t, math::Vec3>> deltas;
                        const bool normals = shape->normal_offsets.count == shape->num_offsets;
                        std::vector<std::pair<std::uint32_t, math::Vec3>> ndeltas;
                        for (std::size_t o = 0; o < shape->num_offsets; ++o) {
                            const math::Vec3 d = vec3(shape->position_offsets.data[o]);
                            const math::Vec3 nd = normals ? vec3(shape->normal_offsets.data[o]) : math::Vec3{};
                            if (!finite3(d) || !finite3(nd)) continue;
                            auto [a, b] = by_source.equal_range(shape->offset_vertices.data[o]);
                            for (auto e = a; e != b; ++e) {
                                deltas.emplace_back(e->second, d);
                                ndeltas.emplace_back(e->second, nd);
                            }
                        }
                        std::vector<std::size_t> order(deltas.size());
                        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
                        std::sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) { return deltas[x].first < deltas[y].first; });
                        for (const std::size_t i : order) {
                            target.vertices.push_back(deltas[i].first);
                            target.positions.push_back(deltas[i].second);
                            if (normals) target.normals.push_back(ndeltas[i].second);
                        }
                    }
                    out.data.morphs.push_back(std::move(target));
                    out.data.morph_weights.push_back(std::isfinite(ch->weight) ? static_cast<float>(ch->weight) : 0.0f);
                }
            }
            if (!mesh.vertex_normal.exists) render::compute_normals(out.data);
            if (settings_.generate_tangents && mesh.vertex_uv.exists) render::compute_tangents(out.data);
            out.material = pi < mats.size() ? material(mats[pi]) : (mats.empty() ? default_material() : material(mats.back()));
            if (settings_.optimize_meshes) {
                render::LodSettings lod;
                lod.max_lods = settings_.lods;
                lod.max_error = settings_.lod_error;
                render::generate_lods(out.data, lod);
                render::optimize_mesh(out.data);
            }
            if (const std::string e = render::validate_morphs(out.data); !e.empty()) return make_error(ErrorCode::ParseError, out.name + ": " + e);
            parts.push_back(static_cast<std::uint32_t>(model_.meshes.size()));
            model_.meshes.push_back(std::move(out));
        }
        return {};
    }

    void import_animations() {
        if (scene_->anim_stacks.count == 0) return;
        ufbx_bake_opts opts{};
        opts.trim_start_time = true;
        opts.resample_rate = 30.0;
        opts.key_reduction_enabled = true;
        opts.key_reduction_rotation = true;
        std::map<std::string, int> names;
        for (std::size_t si = 0; si < scene_->anim_stacks.count; ++si) {
            const ufbx_anim_stack* stack = scene_->anim_stacks.data[si];
            ufbx_error err{};
            ufbx_baked_anim* bake = ufbx_bake_anim(scene_, stack->anim, &opts, &err);
            std::string name = stack->name.length ? to_string(stack->name) : std::format("clip{}", si);
            if (!bake) {
                warn(std::format("animation '{}': {}", name, to_string(err.description)));
                continue;
            }
            if (const int n = names[name]++; n > 0) name = std::format("{} {}", name, n + 1);
            animation::Clip clip;
            clip.name = std::move(name);
            for (std::size_t ni = 0; ni < bake->nodes.count; ++ni) {
                const ufbx_baked_node& bn = bake->nodes.data[ni];
                if (bn.typed_id >= scene_->nodes.count) continue;
                const auto it = node_map_.find(scene_->nodes.data[bn.typed_id]);
                if (it == node_map_.end()) continue;
                const std::uint32_t node = it->second;
                const bool root = model_.nodes[node].parent < 0;
                const float s = root ? settings_.scale : 1.0f;
                auto vec_channel = [&](const ufbx_baked_vec3_list& keys, animation::Path path, bool constant, math::Vec3 rest) {
                    if (keys.count == 0) return;
                    animation::Channel ch;
                    ch.node = node;
                    ch.path = path;
                    for (std::size_t k = 0; k < keys.count; ++k) {
                        const math::Vec3 v = vec3(keys.data[k].value) * s;
                        if (constant && k == 0 && math::length(v - rest) < 1e-5f) return; // matches the rest pose
                        if (!finite3(v)) return;
                        ch.times.push_back(static_cast<float>(keys.data[k].time));
                        ch.values.insert(ch.values.end(), {v.x, v.y, v.z});
                        if (constant) break;
                    }
                    clip.channels.push_back(std::move(ch));
                };
                const ModelNode& mn = model_.nodes[node];
                vec_channel(bn.translation_keys, animation::Path::Translation, bn.constant_translation, mn.translation);
                vec_channel(bn.scale_keys, animation::Path::Scale, bn.constant_scale, mn.scale);
                if (bn.rotation_keys.count) {
                    animation::Channel ch;
                    ch.node = node;
                    ch.path = animation::Path::Rotation;
                    const std::size_t n = bn.constant_rotation ? 1 : bn.rotation_keys.count;
                    for (std::size_t k = 0; k < n; ++k) {
                        const math::Quat q = quat(bn.rotation_keys.data[k].value);
                        ch.times.push_back(static_cast<float>(bn.rotation_keys.data[k].time));
                        ch.values.insert(ch.values.end(), {q.x, q.y, q.z, q.w});
                    }
                    const math::Quat r = mn.rotation;
                    const bool rest = bn.constant_rotation && std::abs(std::abs(ch.values[0] * r.x + ch.values[1] * r.y + ch.values[2] * r.z + ch.values[3] * r.w) - 1.0f) < 1e-6f;
                    if (!rest) clip.channels.push_back(std::move(ch));
                }
            }
            add_weight_channels(*bake, clip);
            float duration = 0.0f;
            for (const auto& ch : clip.channels)
                if (!ch.times.empty()) duration = std::max(duration, ch.times.back());
            clip.duration = std::max(duration, static_cast<float>(bake->playback_duration));
            ufbx_free_baked_anim(bake);
            if (clip.channels.empty()) continue;
            if (auto st = animation::validate(clip, model_.nodes.size()); !st) {
                warn(std::format("animation '{}': {}", clip.name, st.error().message));
                continue;
            }
            model_.animations.push_back(std::move(clip));
        }
    }

    // Blend shape weights ("DeformPercent", 0..100) become one Weights channel per morphing node,
    // sampled at the union of the channels' key times.
    void add_weight_channels(const ufbx_baked_anim& bake, animation::Clip& clip) {
        for (const auto& [mesh, node] : morph_nodes_) {
            std::vector<const ufbx_blend_channel*> channels;
            for (std::size_t d = 0; d < mesh->blend_deformers.count; ++d)
                for (std::size_t c = 0; c < mesh->blend_deformers.data[d]->channels.count; ++c)
                    channels.push_back(mesh->blend_deformers.data[d]->channels.data[c]);
            if (channels.size() > render::kMaxMorphTargets) channels.resize(render::kMaxMorphTargets);
            std::vector<const ufbx_baked_vec3_list*> keys(channels.size(), nullptr);
            std::vector<float> times;
            bool any = false;
            for (std::size_t c = 0; c < channels.size(); ++c) {
                for (std::size_t e = 0; e < bake.elements.count; ++e) {
                    const ufbx_baked_element& el = bake.elements.data[e];
                    if (el.element_id != channels[c]->element_id) continue;
                    for (std::size_t p = 0; p < el.props.count; ++p)
                        if (to_string(el.props.data[p].name) == "DeformPercent" && el.props.data[p].keys.count) {
                            keys[c] = &el.props.data[p].keys;
                            any = true;
                            for (std::size_t k = 0; k < keys[c]->count; ++k) times.push_back(static_cast<float>(keys[c]->data[k].time));
                        }
                }
            }
            if (!any) continue;
            std::sort(times.begin(), times.end());
            times.erase(std::unique(times.begin(), times.end()), times.end());
            animation::Channel ch;
            ch.node = node;
            ch.path = animation::Path::Weights;
            ch.width = static_cast<std::uint32_t>(channels.size());
            ch.times = times;
            const auto& defaults = model_.meshes[model_.nodes[node].meshes[0]].data.morph_weights;
            for (const float t : times)
                for (std::size_t c = 0; c < channels.size(); ++c) {
                    float w = c < defaults.size() ? defaults[c] : 0.0f;
                    if (const ufbx_baked_vec3_list* kl = keys[c]) {
                        std::size_t k = 0;
                        while (k + 1 < kl->count && kl->data[k + 1].time <= t) ++k;
                        double v = kl->data[k].value.x;
                        if (k + 1 < kl->count && t > kl->data[k].time) {
                            const double a = (t - kl->data[k].time) / (kl->data[k + 1].time - kl->data[k].time);
                            v = v + (kl->data[k + 1].value.x - v) * std::clamp(a, 0.0, 1.0);
                        }
                        w = std::isfinite(v) ? static_cast<float>(v / 100.0) : 0.0f;
                    }
                    ch.values.push_back(w);
                }
            clip.channels.push_back(std::move(ch));
        }
    }

    const ufbx_scene* scene_;
    fs::Path path_;
    const ModelImportSettings& settings_;
    FileContext& files_;
    ImportResult& result_;
    CookedModel model_;
    std::int32_t default_material_{-1};
    std::map<const ufbx_node*, std::uint32_t> node_map_;
    std::vector<std::pair<const ufbx_node*, std::uint32_t>> mesh_nodes_;
    std::vector<std::pair<const ufbx_mesh*, std::uint32_t>> morph_nodes_;
    std::map<const ufbx_light*, std::int32_t> light_map_;
    std::map<const ufbx_camera*, std::int32_t> camera_map_;
    std::map<const ufbx_material*, std::uint32_t> materials_;
    std::map<std::tuple<const ufbx_texture*, bool, float>, std::int32_t> textures_;
    std::map<const ufbx_skin_deformer*, std::int32_t> skin_map_;
    std::map<const ufbx_skin_deformer*, std::int32_t> rigid_joint_;
};

Result<CookedModel> import_ufbx_bytes(std::span<const std::uint8_t> bytes, const fs::Path& path, const ModelImportSettings& settings,
                                      FileContext& files, ImportResult& result) {
    ufbx_load_opts opts{};
    opts.target_axes = {UFBX_COORDINATE_AXIS_POSITIVE_X, UFBX_COORDINATE_AXIS_POSITIVE_Y, UFBX_COORDINATE_AXIS_POSITIVE_Z};
    // ufbx "front" is the opposite of forward: cameras and lights end up looking along -Z.
    opts.target_camera_axes = opts.target_axes;
    opts.target_light_axes = opts.target_axes;
    opts.target_unit_meters = 1.0;
    opts.obj_unit_meters = 1.0; // OBJ has no units; treat them as metres
    opts.obj_axes = opts.target_axes;
    opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
    opts.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_COMPENSATE;
    opts.pivot_handling = UFBX_PIVOT_HANDLING_ADJUST_TO_PIVOT;
    opts.generate_missing_normals = true;
    opts.normalize_normals = true;
    opts.load_external_files = !files.memory_only;
    opts.ignore_missing_external_files = true;
    opts.obj_search_mtl_by_filename = true;
    opts.open_file_cb.fn = &sandboxed_open;
    opts.open_file_cb.user = &files;
    const std::string name = path.string();
    opts.filename = {name.c_str(), name.size()};
    ufbx_error err{};
    ufbx_scene* scene = ufbx_load_memory(bytes.data(), bytes.size(), &opts, &err);
    if (!scene) {
        char buf[512];
        ufbx_format_error(buf, sizeof buf, &err);
        return make_error(ErrorCode::ParseError, std::format("{}: {}", path.filename().string(), buf));
    }
    for (std::size_t i = 0; i < scene->metadata.warnings.count; ++i)
        result.warnings.push_back(to_string(scene->metadata.warnings.data[i].description));
    FbxImporter importer(scene, path, settings, files, result);
    auto model = importer.run();
    ufbx_free_scene(scene);
    return model;
}

} // namespace

Result<CookedModel> import_fbx(const fs::Path& path, const ModelImportSettings& settings, const fs::Path& sandbox_root, ImportResult* result) {
    ImportResult local;
    ImportResult& res = result ? *result : local;
    std::vector<fs::Path> deps;
    FileContext files{sandbox_root, &deps, &res.warnings, false};
    auto bytes = fs::read_binary(path, kMaxFbxBytes);
    if (!bytes) return std::move(bytes).error();
    auto model = import_ufbx_bytes(*bytes, path, settings, files, res);
    std::sort(deps.begin(), deps.end());
    deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
    res.dependencies = std::move(deps);
    return model;
}

Result<CookedModel> import_fbx_memory(std::span<const std::uint8_t> bytes, std::string_view name, const ModelImportSettings& settings,
                                      ImportResult* result) {
    ImportResult local;
    ImportResult& res = result ? *result : local;
    FileContext files{{}, nullptr, &res.warnings, true};
    return import_ufbx_bytes(bytes, utf8_path(name), settings, files, res);
}

} // namespace victus::assets
