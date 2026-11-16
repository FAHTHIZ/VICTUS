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
        if (!v->is_string() || !Uuid::parse(v->as_string(), out)) return fail(key, "a texture asset UUID or null");
        return true;
    }
    bool fail(const char* key, std::string_view expected) {
        error = std::format("material field '{}' must be {}", key, expected);
        return false;
    }
};

} // namespace

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
    return v;
}

Result<MaterialAsset> MaterialAsset::from_json(const json::Value& v) {
    if (!v.is_object()) return make_error(ErrorCode::ParseError, "a material must be a JSON object");
    const auto version = v["victus_material"].as_int(0);
    if (version != kMaterialVersion)
        return make_error(version > kMaterialVersion ? ErrorCode::VersionMismatch : ErrorCode::ParseError,
                          std::format("not a VICTUS material (victus_material = {}, expected {})", version, kMaterialVersion));
    MaterialAsset m;
    Reader r{v, {}};
    float emissive[3] = {m.emissive.r, m.emissive.g, m.emissive.b};
    const bool ok = r.vec("base_color", &m.base_color.r, 3, 4) && r.uuid("base_color_texture", m.base_color_texture) &&
                    r.number("metallic", m.metallic) && r.number("roughness", m.roughness) &&
                    r.uuid("metallic_roughness_texture", m.metallic_roughness_texture) &&
                    r.uuid("normal_texture", m.normal_texture) && r.number("normal_scale", m.normal_scale) &&
                    r.vec("emissive", emissive, 3, 4) && r.number("emissive_strength", m.emissive_strength) &&
                    r.uuid("emissive_texture", m.emissive_texture) && r.number("alpha_cutoff", m.alpha_cutoff) &&
                    r.vec("uv_scale", &m.uv_scale.x, 2, 2) && r.vec("uv_offset", &m.uv_offset.x, 2, 2) &&
                    r.boolean("unlit", m.unlit) && r.boolean("double_sided", m.double_sided);
    if (!ok) return make_error(ErrorCode::ParseError, r.error);
    m.emissive = {emissive[0], emissive[1], emissive[2], 1.0f};
    if (const Value* f = v.find("filter"); f && !f->is_null()) {
        if (f->is_string() && f->as_string() == "nearest") m.filter = TextureFilter::Nearest;
        else if (f->is_string() && f->as_string() == "linear") m.filter = TextureFilter::Linear;
        else return make_error(ErrorCode::ParseError, "material field 'filter' must be \"linear\" or \"nearest\"");
    }
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
