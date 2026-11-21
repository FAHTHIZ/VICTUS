// SPDX-License-Identifier: Apache-2.0
#include "victus/assets/material.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <string>

namespace victus::assets {
namespace {

using json::Value;

// Floats are written with 6 significant digits so hand-typed 0.9 stays 0.9 in the file (not
// 0.8999999761581421) and diffs of edited materials stay readable.
Value num(float f) { return Value(std::strtod(std::format("{:.6g}", f).c_str(), nullptr)); }
Value floats(std::initializer_list<float> list) {
    Value a = Value::array();
    for (float f : list) a.push(num(f));
    return a;
}
Value uuid_or_null(const Uuid& u) { return u.is_nil() ? Value() : Value(u.to_string()); }

struct Reader {
    const Value& doc;
    std::string error;

    bool number(const char* key, float& out) {
        const Value* v = doc.find(key);
        if (!v || v->is_null()) return true;
        if (!v->is_number()) return fail(key, "a number");
        out = v->as_float();
        return true;
    }
    bool boolean(const char* key, bool& out) {
        const Value* v = doc.find(key);
        if (!v || v->is_null()) return true;
        if (!v->is_bool()) return fail(key, "true or false");
        out = v->as_bool();
        return true;
    }
    bool vec(const char* key, float* out, std::size_t min, std::size_t max) {
        const Value* v = doc.find(key);
        if (!v || v->is_null()) return true;
        if (!v->is_array() || v->size() < min || v->size() > max) return fail(key, std::format("an array of {}-{} numbers", min, max));
        for (std::size_t i = 0; i < v->size(); ++i) {
            if (!(*v)[i].is_number()) return fail(key, "numbers");
            out[i] = (*v)[i].as_float();
        }
        return true;
    }
    bool uuid(const char* key, Uuid& out) {
        const Value* v = doc.find(key);
        if (!v || v->is_null()) {
            out = {};
            return true;
        }
        if (!v->is_string() || !Uuid::parse(v->as_string(), out)) return fail(key, "an asset UUID or null");
        return true;
    }
    bool fail(const char* key, std::string_view expected) {
        error = std::format("material field '{}' must be {}", key, expected);
        return false;
    }
};

} // namespace

const char* field_key(MaterialField f) noexcept {
    switch (f) {
    case MaterialField::BaseColor: return "base_color";
    case MaterialField::BaseColorTexture: return "base_color_texture";
    case MaterialField::Metallic: return "metallic";
    case MaterialField::Roughness: return "roughness";
    case MaterialField::MetallicRoughnessTexture: return "metallic_roughness_texture";
    case MaterialField::NormalTexture: return "normal_texture";
    case MaterialField::NormalScale: return "normal_scale";
    case MaterialField::Emissive: return "emissive";
    case MaterialField::EmissiveStrength: return "emissive_strength";
    case MaterialField::EmissiveTexture: return "emissive_texture";
    case MaterialField::AlphaCutoff: return "alpha_cutoff";
    case MaterialField::UvScale: return "uv_scale";
    case MaterialField::UvOffset: return "uv_offset";
    case MaterialField::Filter: return "filter";
    case MaterialField::Unlit: return "unlit";
    case MaterialField::DoubleSided: return "double_sided";
    case MaterialField::Shader: return "shader";
    case MaterialField::ShaderParams: return "shader_params";
    case MaterialField::VertexAlpha: return "vertex_alpha";
    case MaterialField::CoverageResponse: return "coverage_response";
    case MaterialField::Blend: return "blend";
    case MaterialField::OcclusionStrength: return "occlusion_strength";
    case MaterialField::ShaderTextures: return "shader_textures";
    case MaterialField::Responsive: return "responsive";
    default: return "";
    }
}

void MaterialAsset::copy_field(const MaterialAsset& src, MaterialField f) {
    switch (f) {
    case MaterialField::BaseColor: base_color = src.base_color; break;
    case MaterialField::BaseColorTexture: base_color_texture = src.base_color_texture; break;
    case MaterialField::Metallic: metallic = src.metallic; break;
    case MaterialField::Roughness: roughness = src.roughness; break;
    case MaterialField::MetallicRoughnessTexture: metallic_roughness_texture = src.metallic_roughness_texture; break;
    case MaterialField::NormalTexture: normal_texture = src.normal_texture; break;
    case MaterialField::NormalScale: normal_scale = src.normal_scale; break;
    case MaterialField::Emissive: emissive = src.emissive; break;
    case MaterialField::EmissiveStrength: emissive_strength = src.emissive_strength; break;
    case MaterialField::EmissiveTexture: emissive_texture = src.emissive_texture; break;
    case MaterialField::AlphaCutoff: alpha_cutoff = src.alpha_cutoff; break;
    case MaterialField::UvScale: uv_scale = src.uv_scale; break;
    case MaterialField::UvOffset: uv_offset = src.uv_offset; break;
    case MaterialField::Filter: filter = src.filter; break;
    case MaterialField::Unlit: unlit = src.unlit; break;
    case MaterialField::DoubleSided: double_sided = src.double_sided; break;
    case MaterialField::Shader: shader = src.shader; break;
    case MaterialField::ShaderParams:
        shader_params = src.shader_params;
        shader_params_set = src.shader_params_set;
        break;
    case MaterialField::ShaderTextures: shader_textures = src.shader_textures; break;
    case MaterialField::VertexAlpha: vertex_alpha_coverage_mask = src.vertex_alpha_coverage_mask; break;
    case MaterialField::CoverageResponse: coverage_response = src.coverage_response; break;
    case MaterialField::Blend: blend = src.blend; break;
    case MaterialField::OcclusionStrength: occlusion_strength = src.occlusion_strength; break;
    case MaterialField::Responsive: responsive = src.responsive; break;
    default: break;
    }
}

MaterialAsset apply_instance(const MaterialAsset& base, const MaterialAsset& instance) {
    MaterialAsset out = base;
    for (std::size_t i = 0; i < kMaterialFieldCount; ++i) {
        const auto f = static_cast<MaterialField>(i);
        if (instance.overrides_field(f)) out.copy_field(instance, f);
    }
    out.parent = instance.parent;
    out.overrides = instance.overrides;
    return out;
}

json::Value MaterialAsset::to_json() const {
    Value v = Value::object();
    v.set("victus_material", kMaterialVersion);
    v.set("base_color", floats({base_color.r, base_color.g, base_color.b, base_color.a}));
    v.set("base_color_texture", uuid_or_null(base_color_texture));
    v.set("metallic", num(metallic));
    v.set("roughness", num(roughness));
    v.set("metallic_roughness_texture", uuid_or_null(metallic_roughness_texture));
    v.set("normal_texture", uuid_or_null(normal_texture));
    v.set("normal_scale", num(normal_scale));
    v.set("emissive", floats({emissive.r, emissive.g, emissive.b}));
    v.set("emissive_strength", num(emissive_strength));
    v.set("emissive_texture", uuid_or_null(emissive_texture));
    v.set("alpha_cutoff", num(alpha_cutoff));
    v.set("uv_scale", floats({uv_scale.x, uv_scale.y}));
    v.set("uv_offset", floats({uv_offset.x, uv_offset.y}));
    v.set("filter", filter == TextureFilter::Nearest ? "nearest" : "linear");
    v.set("unlit", unlit);
    v.set("double_sided", double_sided);
    v.set("shader", uuid_or_null(shader));
    Value params = Value::array();
    std::size_t used = shader_params_set.count;
    for (std::size_t i = 0; i < shader_params.size(); ++i)
        if (shader_params[i] != math::Vec4{}) used = i + 1;
    for (std::size_t i = 0; i < used; ++i) {
        const math::Vec4& p = shader_params[i];
        params.push(floats({p.x, p.y, p.z, p.w}));
    }
    v.set("shader_params", std::move(params));
    std::size_t textures_used = 0;
    for (std::size_t i = 0; i < shader_textures.size(); ++i)
        if (!shader_textures[i].is_nil()) textures_used = i + 1;
    if (textures_used > 0 || (is_instance() && overrides_field(MaterialField::ShaderTextures))) {
        Value tex = Value::array(); // omitted when empty, so older files round-trip
        for (std::size_t i = 0; i < textures_used; ++i) tex.push(uuid_or_null(shader_textures[i]));
        v.set("shader_textures", std::move(tex));
    }
    v.set("vertex_alpha", vertex_alpha_coverage_mask ? "coverage_mask" : "opacity");
    v.set("coverage_response", num(coverage_response));
    v.set("blend", blend == MaterialBlendMode::Alpha ? "alpha" : blend == MaterialBlendMode::Additive ? "additive" : "opaque");
    if (occlusion_strength != 0.0f || (is_instance() && overrides_field(MaterialField::OcclusionStrength)))
        v.set("occlusion_strength", num(occlusion_strength)); // omitted at the default, so older files round-trip
    if (responsive != 0.0f || (is_instance() && overrides_field(MaterialField::Responsive)))
        v.set("responsive", num(responsive)); // omitted at the default
    if (!is_instance()) return v;
    // Instances store only what they override.
    Value inst = Value::object();
    inst.set("victus_material", kMaterialVersion);
    inst.set("parent", parent.to_string());
    for (std::size_t i = 0; i < kMaterialFieldCount; ++i) {
        const auto f = static_cast<MaterialField>(i);
        if (overrides_field(f))
            if (const Value* field = v.find(field_key(f))) inst.set(field_key(f), *field);
    }
    return inst;
}

Result<MaterialAsset> MaterialAsset::from_json(const json::Value& v) {
    if (!v.is_object()) return make_error(ErrorCode::ParseError, "a material must be a JSON object");
    const auto version = v["victus_material"].as_int(0);
    if (version != kMaterialVersion)
        return make_error(version > kMaterialVersion ? ErrorCode::VersionMismatch : ErrorCode::ParseError,
                          std::format("not a VICTUS material (victus_material = {}, expected {})", version, kMaterialVersion));
    MaterialAsset m;
    Reader r{v, {}};
    float emissive[4] = {m.emissive.r, m.emissive.g, m.emissive.b, 1.0f}; // files may give 3 or 4 values
    const bool ok = r.vec("base_color", &m.base_color.r, 3, 4) && r.uuid("base_color_texture", m.base_color_texture) &&
                    r.number("metallic", m.metallic) && r.number("roughness", m.roughness) &&
                    r.uuid("metallic_roughness_texture", m.metallic_roughness_texture) &&
                    r.uuid("normal_texture", m.normal_texture) && r.number("normal_scale", m.normal_scale) &&
                    r.vec("emissive", emissive, 3, 4) && r.number("emissive_strength", m.emissive_strength) &&
                    r.uuid("emissive_texture", m.emissive_texture) && r.number("alpha_cutoff", m.alpha_cutoff) &&
                    r.vec("uv_scale", &m.uv_scale.x, 2, 2) && r.vec("uv_offset", &m.uv_offset.x, 2, 2) &&
                    r.boolean("unlit", m.unlit) && r.boolean("double_sided", m.double_sided) &&
                    r.number("coverage_response", m.coverage_response) && r.number("occlusion_strength", m.occlusion_strength) && r.number("responsive", m.responsive);
    if (!ok) return make_error(ErrorCode::ParseError, r.error);
    m.emissive = {emissive[0], emissive[1], emissive[2], 1.0f};
    if (!r.uuid("shader", m.shader)) return make_error(ErrorCode::ParseError, r.error);
    if (const Value* p = v.find("shader_params"); p && !p->is_null()) {
        if (!p->is_array() || p->size() > kMaterialShaderParams)
            return make_error(ErrorCode::ParseError, "material field 'shader_params' must be an array of up to 4 vectors");
        for (std::size_t i = 0; i < p->size(); ++i) {
            const Value& e = (*p)[i];
            if (!e.is_array() || e.size() < 1 || e.size() > 4)
                return make_error(ErrorCode::ParseError, "material field 'shader_params' entries must be arrays of 1-4 numbers");
            float out[4] = {0, 0, 0, 0};
            for (std::size_t c = 0; c < e.size(); ++c) {
                if (!e[c].is_number()) return make_error(ErrorCode::ParseError, "material field 'shader_params' entries must be numbers");
                out[c] = e[c].as_float();
            }
            m.shader_params[i] = {out[0], out[1], out[2], out[3]};
        }
        m.shader_params_set.count = static_cast<std::uint8_t>(p->size());
    }
    if (const Value* t = v.find("shader_textures"); t && !t->is_null()) {
        if (!t->is_array() || t->size() > m.shader_textures.size())
            return make_error(ErrorCode::ParseError, "material field 'shader_textures' must be an array of up to 4 asset UUIDs or nulls");
        for (std::size_t i = 0; i < t->size(); ++i) {
            const Value& e = (*t)[i];
            if (e.is_null()) continue;
            if (!e.is_string() || !Uuid::parse(e.as_string(), m.shader_textures[i]))
                return make_error(ErrorCode::ParseError, "material field 'shader_textures' entries must be asset UUIDs or null");
        }
    }
    if (const Value* f = v.find("filter"); f && !f->is_null()) {
        if (f->is_string() && f->as_string() == "nearest") m.filter = TextureFilter::Nearest;
        else if (f->is_string() && f->as_string() == "linear") m.filter = TextureFilter::Linear;
        else return make_error(ErrorCode::ParseError, "material field 'filter' must be \"linear\" or \"nearest\"");
    }
    if (const Value* f = v.find("vertex_alpha"); f && !f->is_null()) {
        if (f->is_string() && f->as_string() == "coverage_mask") m.vertex_alpha_coverage_mask = true;
        else if (f->is_string() && f->as_string() == "opacity") m.vertex_alpha_coverage_mask = false;
        else return make_error(ErrorCode::ParseError, "material field 'vertex_alpha' must be \"opacity\" or \"coverage_mask\"");
    }
    if (!r.uuid("parent", m.parent)) return make_error(ErrorCode::ParseError, r.error);
    m.overrides = 0;
    for (std::size_t i = 0; i < kMaterialFieldCount; ++i)
        if (v.find(field_key(static_cast<MaterialField>(i)))) m.overrides |= field_bit(static_cast<MaterialField>(i));
    if (!m.is_instance()) m.overrides = kAllMaterialFields;
    if (const Value* f = v.find("blend"); f && !f->is_null()) {
        const std::string_view b = f->is_string() ? std::string_view{f->as_string()} : std::string_view{};
        if (b == "opaque") m.blend = MaterialBlendMode::Opaque;
        else if (b == "alpha") m.blend = MaterialBlendMode::Alpha;
        else if (b == "additive") m.blend = MaterialBlendMode::Additive;
        else return make_error(ErrorCode::ParseError, "material field 'blend' must be \"opaque\", \"alpha\" or \"additive\"");
    }
    m.coverage_response = std::clamp(m.coverage_response, 0.0f, 1.0f);
    m.occlusion_strength = std::clamp(m.occlusion_strength, 0.0f, 1.0f);
    m.responsive = std::clamp(m.responsive, 0.0f, 1.0f);
    m.metallic = std::clamp(m.metallic, 0.0f, 1.0f);
    m.roughness = std::clamp(m.roughness, 0.0f, 1.0f);
    m.alpha_cutoff = std::clamp(m.alpha_cutoff, 0.0f, 1.0f);
    m.emissive_strength = std::max(m.emissive_strength, 0.0f);
    return m;
}

Result<MaterialAsset> load_material_file(const fs::Path& path) {
    auto text = fs::read_text(path, 1u << 20);
    if (!text) return std::move(text).error();
    auto doc = json::parse(*text);
    if (!doc) return make_error(ErrorCode::ParseError, std::format("{}: {}", path.filename().string(), doc.error().message));
    auto m = MaterialAsset::from_json(*doc);
    if (!m) return make_error(m.error().code, std::format("{}: {}", path.filename().string(), m.error().message));
    return m;
}

Status save_material_file(const fs::Path& path, const MaterialAsset& material) {
    return fs::write_text_atomic(path, json::stringify(material.to_json(), true) + "\n");
}

} // namespace victus::assets
