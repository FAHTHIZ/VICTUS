// SPDX-License-Identifier: Apache-2.0
// Minimal glTF 2.0 writer for cooked models: geometry (positions, normals, UV0, tangents,
// JOINTS_0/WEIGHTS_0), material factors, node hierarchy, skins and animations, with one
// base64-embedded buffer. Textures are not exported. Used for round-trip tests and generated
// template content; the output re-imports through import_gltf.
#include "victus/assets/importers.hpp"
#include "victus/core/hash.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <format>
#include <map>

namespace victus::assets {

namespace {

constexpr int kFloat = 5126, kUShort = 5123, kUInt = 5125;
constexpr int kArrayBuffer = 34962, kElementArrayBuffer = 34963;

json::Value floats(const std::vector<float>& v) {
    json::Value a = json::Value::array();
    for (const float f : v) a.push(f);
    return a;
}

json::Value floats(std::initializer_list<float> v) {
    json::Value a = json::Value::array();
    for (const float f : v) a.push(f);
    return a;
}

class Writer {
public:
    // Appends `bytes` as a new buffer view (4-byte aligned) and returns its index.
    std::uint32_t view(const void* data, std::size_t size, int target) {
        while (buffer_.size() % 4) buffer_.push_back(0);
        json::Value v = json::Value::object();
        v.set("buffer", 0);
        v.set("byteOffset", static_cast<std::uint64_t>(buffer_.size()));
        v.set("byteLength", static_cast<std::uint64_t>(size));
        if (target) v.set("target", target);
        const auto* p = static_cast<const std::uint8_t*>(data);
        buffer_.insert(buffer_.end(), p, p + size);
        views_.push(std::move(v));
        return static_cast<std::uint32_t>(views_.size() - 1);
    }
    std::uint32_t accessor(std::uint32_t view, std::size_t offset, int component, std::size_t count, const char* type,
                           json::Value min = {}, json::Value max = {}, std::size_t stride = 0) {
        json::Value a = json::Value::object();
        a.set("bufferView", view);
        if (offset) a.set("byteOffset", static_cast<std::uint64_t>(offset));
        a.set("componentType", component);
        a.set("count", static_cast<std::uint64_t>(count));
        a.set("type", type);
        if (!min.is_null()) a.set("min", std::move(min));
        if (!max.is_null()) a.set("max", std::move(max));
        if (stride) stride_[view] = stride;
        accessors_.push(std::move(a));
        return static_cast<std::uint32_t>(accessors_.size() - 1);
    }
    std::uint32_t float_accessor(const std::vector<float>& values, const char* type, bool with_range = false) {
        const std::uint32_t v = view(values.data(), values.size() * sizeof(float), 0);
        json::Value mn, mx;
        const std::size_t comps = std::strcmp(type, "SCALAR") == 0 ? 1 : std::strcmp(type, "VEC3") == 0 ? 3 : std::strcmp(type, "VEC4") == 0 ? 4 : 16;
        if (with_range && values.size() >= comps) {
            // Per-component bounds (glTF requires them on POSITION, including morph targets).
            std::vector<float> lo(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(comps)), hi = lo;
            for (std::size_t i = 0; i < values.size(); ++i) {
                lo[i % comps] = std::min(lo[i % comps], values[i]);
                hi[i % comps] = std::max(hi[i % comps], values[i]);
            }
            mn = floats(lo);
            mx = floats(hi);
        }
        return accessor(v, 0, kFloat, values.size() / comps, type, std::move(mn), std::move(mx));
    }
    json::Value finish(json::Value doc) {
        for (const auto& [view, stride] : stride_) {
            json::Value& v = views_.as_array()[view];
            v.set("byteStride", static_cast<std::uint64_t>(stride));
        }
        json::Value buffer = json::Value::object();
        buffer.set("byteLength", static_cast<std::uint64_t>(buffer_.size()));
        buffer.set("uri", "data:application/octet-stream;base64," + base64_encode(buffer_));
        json::Value buffers = json::Value::array();
        buffers.push(std::move(buffer));
        doc.set("buffers", std::move(buffers));
        doc.set("bufferViews", std::move(views_));
        doc.set("accessors", std::move(accessors_));
        return doc;
    }

private:
    std::vector<std::uint8_t> buffer_;
    json::Value views_ = json::Value::array();
    json::Value accessors_ = json::Value::array();
    std::map<std::uint32_t, std::size_t> stride_;
};

json::Value primitive(Writer& w, const ModelMesh& mesh) {
    const auto& verts = mesh.data.vertices;
    const std::uint32_t vv = w.view(verts.data(), verts.size() * sizeof(render::Vertex3D), kArrayBuffer);
    math::Vec3 lo = verts.empty() ? math::Vec3{} : verts[0].position, hi = lo;
    for (const auto& v : verts) {
        lo = {std::min(lo.x, v.position.x), std::min(lo.y, v.position.y), std::min(lo.z, v.position.z)};
        hi = {std::max(hi.x, v.position.x), std::max(hi.y, v.position.y), std::max(hi.z, v.position.z)};
    }
    constexpr std::size_t stride = sizeof(render::Vertex3D);
    json::Value attrs = json::Value::object();
    attrs.set("POSITION", w.accessor(vv, offsetof(render::Vertex3D, position), kFloat, verts.size(), "VEC3", floats({lo.x, lo.y, lo.z}),
                                     floats({hi.x, hi.y, hi.z}), stride));
    attrs.set("NORMAL", w.accessor(vv, offsetof(render::Vertex3D, normal), kFloat, verts.size(), "VEC3", {}, {}, stride));
    attrs.set("TEXCOORD_0", w.accessor(vv, offsetof(render::Vertex3D, uv), kFloat, verts.size(), "VEC2", {}, {}, stride));
    attrs.set("TANGENT", w.accessor(vv, offsetof(render::Vertex3D, tangent), kFloat, verts.size(), "VEC4", {}, {}, stride));
    // COLOR_0 only when some vertex is not white; glTF colours are linear floats.
    bool any_color = false;
    for (const auto& v : verts) any_color = any_color || v.color != 0xFFFFFFFFu;
    if (any_color) {
        std::vector<float> lin;
        lin.reserve(verts.size() * 4);
        for (const auto& v : verts) {
            const math::Color c = render::unpack_vertex_color(v.color);
            lin.insert(lin.end(), {c.r, c.g, c.b, c.a});
        }
        const std::uint32_t cv = w.view(lin.data(), lin.size() * sizeof(float), kArrayBuffer);
        attrs.set("COLOR_0", w.accessor(cv, 0, kFloat, verts.size(), "VEC4"));
    }
    if (mesh.data.skinned()) {
        const auto& skin = mesh.data.skin;
        const std::uint32_t sv = w.view(skin.data(), skin.size() * sizeof(render::SkinVertex), kArrayBuffer);
        constexpr std::size_t sstride = sizeof(render::SkinVertex);
        attrs.set("JOINTS_0", w.accessor(sv, offsetof(render::SkinVertex, joints), kUShort, skin.size(), "VEC4", {}, {}, sstride));
        attrs.set("WEIGHTS_0", w.accessor(sv, offsetof(render::SkinVertex, weights), kFloat, skin.size(), "VEC4", {}, {}, sstride));
    }
    const auto& idx = mesh.data.indices;
    const std::uint32_t iv = w.view(idx.data(), idx.size() * sizeof(std::uint32_t), kElementArrayBuffer);
    json::Value p = json::Value::object();
    p.set("attributes", std::move(attrs));
    if (!mesh.data.morphs.empty()) {
        // Dense deltas (glTF targets index every vertex); sparse storage is ours only.
        json::Value targets = json::Value::array();
        for (const render::MorphTarget& t : mesh.data.morphs) {
            std::vector<float> dp(verts.size() * 3, 0.0f), dn;
            if (!t.normals.empty()) dn.assign(verts.size() * 3, 0.0f);
            for (std::size_t k = 0; k < t.vertices.size(); ++k) {
                const std::size_t v = t.vertices[k];
                dp[v * 3] = t.positions[k].x, dp[v * 3 + 1] = t.positions[k].y, dp[v * 3 + 2] = t.positions[k].z;
                if (!dn.empty()) dn[v * 3] = t.normals[k].x, dn[v * 3 + 1] = t.normals[k].y, dn[v * 3 + 2] = t.normals[k].z;
            }
            json::Value target = json::Value::object();
            target.set("POSITION", w.float_accessor(dp, "VEC3", true));
            if (!dn.empty()) target.set("NORMAL", w.float_accessor(dn, "VEC3"));
            targets.push(std::move(target));
        }
        p.set("targets", std::move(targets));
    }
    p.set("indices", w.accessor(iv, 0, kUInt, idx.size(), "SCALAR"));
    p.set("material", mesh.material);
    return p;
}

const char* path_name(animation::Path p) {
    switch (p) {
    case animation::Path::Translation: return "translation";
    case animation::Path::Rotation: return "rotation";
    case animation::Path::Weights: return "weights";
    default: return "scale";
    }
}

const char* interpolation_name(animation::Interpolation i) {
    switch (i) {
    case animation::Interpolation::Step: return "STEP";
    case animation::Interpolation::CubicSpline: return "CUBICSPLINE";
    default: return "LINEAR";
    }
}

} // namespace

Result<std::string> export_gltf_json(const CookedModel& model) {
    if (auto st = validate_model(model); !st) return std::move(st).error();
    Writer w;
    json::Value doc = json::Value::object();
    json::Value asset = json::Value::object();
    asset.set("version", "2.0");
    asset.set("generator", "VICTUS export_gltf");
    doc.set("asset", std::move(asset));

    json::Value materials = json::Value::array();
    for (const ModelMaterial& m : model.materials) {
        json::Value mat = json::Value::object();
        mat.set("name", m.name);
        json::Value pbr = json::Value::object();
        pbr.set("baseColorFactor", floats({m.base_color.r, m.base_color.g, m.base_color.b, m.base_color.a}));
        pbr.set("metallicFactor", m.metallic);
        pbr.set("roughnessFactor", m.roughness);
        mat.set("pbrMetallicRoughness", std::move(pbr));
        mat.set("emissiveFactor", floats({m.emissive.r, m.emissive.g, m.emissive.b}));
        if (m.double_sided) mat.set("doubleSided", true);
        if (m.alpha_cutoff > 0.0f) {
            mat.set("alphaMode", "MASK");
            mat.set("alphaCutoff", m.alpha_cutoff);
        }
        materials.push(std::move(mat));
    }
    doc.set("materials", std::move(materials));

    // One glTF mesh per distinct node mesh list (primitives in order).
    json::Value meshes = json::Value::array();
    std::map<std::vector<std::uint32_t>, std::uint32_t> mesh_of;
    std::vector<json::Value> prims(model.meshes.size());
    for (std::size_t i = 0; i < model.meshes.size(); ++i) prims[i] = primitive(w, model.meshes[i]);
    for (const ModelNode& n : model.nodes) {
        if (n.meshes.empty() || mesh_of.contains(n.meshes)) continue;
        json::Value mesh = json::Value::object();
        mesh.set("name", model.meshes[n.meshes[0]].name);
        json::Value list = json::Value::array();
        for (const std::uint32_t mi : n.meshes) list.push(prims[mi]);
        mesh.set("primitives", std::move(list));
        for (const std::uint32_t mi : n.meshes) {
            const render::MeshData& d = model.meshes[mi].data;
            if (d.morphs.empty()) continue;
            std::vector<float> weights = d.morph_weights;
            weights.resize(d.morphs.size(), 0.0f);
            mesh.set("weights", floats(weights));
            json::Value names = json::Value::array();
            for (const auto& t : d.morphs) names.push(t.name);
            json::Value extras = json::Value::object();
            extras.set("targetNames", std::move(names)); // de facto convention read by cgltf and Blender
            mesh.set("extras", std::move(extras));
            break;
        }
        mesh_of[n.meshes] = static_cast<std::uint32_t>(meshes.size());
        meshes.push(std::move(mesh));
    }
    doc.set("meshes", std::move(meshes));

    json::Value nodes = json::Value::array();
    std::vector<json::Value> children(model.nodes.size(), json::Value::array());
    json::Value roots = json::Value::array();
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const std::int32_t p = model.nodes[i].parent;
        if (p < 0) roots.push(static_cast<std::uint32_t>(i));
        else children[static_cast<std::size_t>(p)].push(static_cast<std::uint32_t>(i));
    }
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const ModelNode& n = model.nodes[i];
        json::Value node = json::Value::object();
        node.set("name", n.name);
        node.set("translation", floats({n.translation.x, n.translation.y, n.translation.z}));
        node.set("rotation", floats({n.rotation.x, n.rotation.y, n.rotation.z, n.rotation.w}));
        node.set("scale", floats({n.scale.x, n.scale.y, n.scale.z}));
        if (!n.meshes.empty()) node.set("mesh", mesh_of[n.meshes]);
        if (n.skin >= 0) node.set("skin", n.skin);
        if (!n.morph_weights.empty()) node.set("weights", floats(n.morph_weights));
        if (n.camera >= 0) node.set("camera", n.camera);
        if (n.light >= 0) {
            json::Value ref = json::Value::object();
            ref.set("light", n.light);
            json::Value ext = json::Value::object();
            ext.set("KHR_lights_punctual", std::move(ref));
            node.set("extensions", std::move(ext));
        }
        if (children[i].size()) node.set("children", std::move(children[i]));
        nodes.push(std::move(node));
    }
    doc.set("nodes", std::move(nodes));
    if (!model.cameras.empty()) {
        json::Value cams = json::Value::array();
        for (const ModelCamera& c : model.cameras) {
            json::Value cam = json::Value::object();
            cam.set("name", c.name);
            json::Value p = json::Value::object();
            if (c.orthographic) {
                cam.set("type", "orthographic");
                p.set("ymag", c.ymag);
                p.set("xmag", c.ymag * (c.aspect > 0.0f ? c.aspect : 1.0f));
                p.set("znear", c.znear);
                p.set("zfar", c.zfar > c.znear ? c.zfar : c.znear + 100.0f); // required for orthographic
                cam.set("orthographic", std::move(p));
            } else {
                cam.set("type", "perspective");
                p.set("yfov", c.yfov);
                p.set("znear", c.znear);
                if (c.zfar > 0.0f) p.set("zfar", c.zfar);
                if (c.aspect > 0.0f) p.set("aspectRatio", c.aspect);
                cam.set("perspective", std::move(p));
            }
            cams.push(std::move(cam));
        }
        doc.set("cameras", std::move(cams));
    }
    if (!model.lights.empty()) {
        // Photometric units (the importer's default light_intensity_scale divides by 683 again).
        json::Value lights = json::Value::array();
        for (const ModelLight& l : model.lights) {
            json::Value light = json::Value::object();
            light.set("name", l.name);
            light.set("type", l.type == ModelLight::Type::Directional ? "directional" : l.type == ModelLight::Type::Spot ? "spot" : "point");
            light.set("color", floats({l.color.r, l.color.g, l.color.b}));
            light.set("intensity", l.intensity * 683.0f);
            if (l.range > 0.0f && l.type != ModelLight::Type::Directional) light.set("range", l.range);
            if (l.type == ModelLight::Type::Spot) {
                json::Value spot = json::Value::object();
                spot.set("innerConeAngle", l.inner_cone);
                spot.set("outerConeAngle", l.outer_cone);
                light.set("spot", std::move(spot));
            }
            lights.push(std::move(light));
        }
        json::Value khr = json::Value::object();
        khr.set("lights", std::move(lights));
        json::Value ext = json::Value::object();
        ext.set("KHR_lights_punctual", std::move(khr));
        doc.set("extensions", std::move(ext));
        json::Value used = json::Value::array();
        used.push("KHR_lights_punctual");
        doc.set("extensionsUsed", std::move(used));
    }
    json::Value scene = json::Value::object();
    scene.set("nodes", std::move(roots));
    json::Value scenes = json::Value::array();
    scenes.push(std::move(scene));
    doc.set("scenes", std::move(scenes));
    doc.set("scene", 0);

    if (!model.skins.empty()) {
        json::Value skins = json::Value::array();
        for (const ModelSkin& s : model.skins) {
            json::Value skin = json::Value::object();
            skin.set("name", s.name);
            json::Value joints = json::Value::array();
            for (const std::uint32_t j : s.joints) joints.push(j);
            skin.set("joints", std::move(joints));
            std::vector<float> m(s.inverse_bind.size() * 16);
            std::memcpy(m.data(), s.inverse_bind.data(), m.size() * sizeof(float));
            skin.set("inverseBindMatrices", w.float_accessor(m, "MAT4"));
            skins.push(std::move(skin));
        }
        doc.set("skins", std::move(skins));
    }

    if (!model.animations.empty()) {
        json::Value anims = json::Value::array();
        for (const animation::Clip& clip : model.animations) {
            json::Value samplers = json::Value::array(), channels = json::Value::array();
            for (const animation::Channel& ch : clip.channels) {
                json::Value sampler = json::Value::object();
                sampler.set("input", w.float_accessor(ch.times, "SCALAR", true));
                sampler.set("output", w.float_accessor(ch.values, ch.path == animation::Path::Rotation  ? "VEC4"
                                                                    : ch.path == animation::Path::Weights ? "SCALAR"
                                                                                                          : "VEC3"));
                sampler.set("interpolation", interpolation_name(ch.interpolation));
                json::Value target = json::Value::object();
                target.set("node", ch.node);
                target.set("path", path_name(ch.path));
                json::Value channel = json::Value::object();
                channel.set("sampler", static_cast<std::uint32_t>(samplers.size()));
                channel.set("target", std::move(target));
                samplers.push(std::move(sampler));
                channels.push(std::move(channel));
            }
            json::Value a = json::Value::object();
            a.set("name", clip.name);
            a.set("samplers", std::move(samplers));
            a.set("channels", std::move(channels));
            anims.push(std::move(a));
        }
        doc.set("animations", std::move(anims));
    }
    return json::stringify(w.finish(std::move(doc)));
}

Status export_gltf(const CookedModel& model, const fs::Path& path) {
    auto text = export_gltf_json(model);
    if (!text) return std::move(text).error();
    return fs::write_text_atomic(path, *text);
}

} // namespace victus::assets
