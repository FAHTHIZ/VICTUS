// SPDX-License-Identifier: Apache-2.0
// Script bindings: Procedural mesh entity methods (set_mesh_data and friends).
#include "script_internal.hpp"

namespace victus::script::detail {

// ---- procedural meshes ---------------------------------------------------------------------------

// One attribute of a packed vertex layout ("position:f32x3,normal:s8x4,color:u8x4,uv:f32x2,skip:4").
struct PackedAttribute {
    enum class Kind : std::uint8_t { Position, Normal, Uv, Color, Skip } kind{Kind::Skip};
    enum class Type : std::uint8_t { F32, S8, U8, S16, U16, Bytes } type{Type::Bytes};
    std::uint32_t count{0};  // components
    std::uint32_t offset{0}; // bytes from the vertex start
};


std::uint32_t component_bytes(PackedAttribute::Type t) {
    switch (t) {
    case PackedAttribute::Type::F32: return 4;
    case PackedAttribute::Type::S16:
    case PackedAttribute::Type::U16: return 2;
    default: return 1;
    }
}


// Parses a layout string; returns the stride or 0 with `error` set.
std::uint32_t parse_layout(std::string_view text, std::vector<PackedAttribute>& out, std::string& error) {
    out.clear();
    std::uint32_t offset = 0;
    bool position = false;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        std::string_view item = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
        while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
        const std::size_t colon = item.find(':');
        if (colon == std::string_view::npos) {
            error = std::format("layout item '{}' is not name:type", item);
            return 0;
        }
        const std::string_view name = item.substr(0, colon), type = item.substr(colon + 1);
        PackedAttribute a;
        a.offset = offset;
        if (name == "skip") {
            int n = 0;
            for (const char c : type) {
                if (c < '0' || c > '9' || n > 4096) {
                    error = std::format("layout: skip needs a byte count, got '{}'", type);
                    return 0;
                }
                n = n * 10 + (c - '0');
            }
            a.kind = PackedAttribute::Kind::Skip;
            a.count = static_cast<std::uint32_t>(n);
            offset += a.count;
            out.push_back(a);
            continue;
        }
        if (name == "position") a.kind = PackedAttribute::Kind::Position;
        else if (name == "normal") a.kind = PackedAttribute::Kind::Normal;
        else if (name == "uv") a.kind = PackedAttribute::Kind::Uv;
        else if (name == "color") a.kind = PackedAttribute::Kind::Color;
        else {
            error = std::format("layout: unknown attribute '{}' (position, normal, uv, color, skip)", name);
            return 0;
        }
        const std::size_t x = type.find('x');
        const std::string_view base = type.substr(0, x);
        const int count = x == std::string_view::npos || x + 2 != type.size() ? 0 : type[x + 1] - '0';
        if (base == "f32") a.type = PackedAttribute::Type::F32;
        else if (base == "s8") a.type = PackedAttribute::Type::S8;
        else if (base == "u8") a.type = PackedAttribute::Type::U8;
        else if (base == "s16") a.type = PackedAttribute::Type::S16;
        else if (base == "u16") a.type = PackedAttribute::Type::U16;
        const int need = a.kind == PackedAttribute::Kind::Uv ? 2 : 3;
        if (a.type == PackedAttribute::Type::Bytes || count < need || count > 4) {
            error = std::format("layout: '{}' needs a type like f32x{} / s8x4 / u8x4, got '{}'", name, need, type);
            return 0;
        }
        if (a.kind == PackedAttribute::Kind::Color && a.type != PackedAttribute::Type::U8 && a.type != PackedAttribute::Type::F32) {
            error = "layout: color must be u8x3, u8x4, f32x3 or f32x4";
            return 0;
        }
        if (a.kind == PackedAttribute::Kind::Position && a.type != PackedAttribute::Type::F32) {
            error = "layout: position must be f32x3";
            return 0;
        }
        position = position || a.kind == PackedAttribute::Kind::Position;
        a.count = static_cast<std::uint32_t>(count);
        offset += a.count * component_bytes(a.type);
        out.push_back(a);
    }
    if (!position) {
        error = "layout has no position";
        return 0;
    }
    return offset;
}


float read_component(const std::uint8_t* p, PackedAttribute::Type t, std::uint32_t i) {
    switch (t) {
    case PackedAttribute::Type::F32: {
        float f;
        std::memcpy(&f, p + i * 4, 4);
        return std::isfinite(f) ? f : 0.0f;
    }
    case PackedAttribute::Type::S8: return std::max(-1.0f, static_cast<float>(static_cast<std::int8_t>(p[i])) / 127.0f);
    case PackedAttribute::Type::U8: return static_cast<float>(p[i]) / 255.0f;
    case PackedAttribute::Type::S16: {
        std::int16_t v;
        std::memcpy(&v, p + i * 2, 2);
        return std::max(-1.0f, static_cast<float>(v) / 32767.0f);
    }
    case PackedAttribute::Type::U16: {
        std::uint16_t v;
        std::memcpy(&v, p + i * 2, 2);
        return static_cast<float>(v) / 65535.0f;
    }
    default: return 0.0f;
    }
}


std::uint32_t pack_linear_color(float r, float g, float b, float a) {
    const auto enc = [](float c, bool srgb) {
        c = std::clamp(c, 0.0f, 1.0f);
        if (srgb) c = math::linear_to_srgb(c);
        return static_cast<std::uint32_t>(std::lround(c * 255.0f));
    };
    return enc(r, true) | (enc(g, true) << 8) | (enc(b, true) << 16) | (enc(a, false) << 24);
}


// Bytes of a vertices/indices field: a base64 string (encoding "base64", the default) or raw bytes.
std::vector<std::uint8_t> field_bytes(lua_State* L, int spec, const char* field, bool base64) {
    lua_getfield(L, spec, field);
    std::size_t n = 0;
    const char* text = lua_tolstring(L, -1, &n);
    if (!text) luaL_error(L, "set_mesh_data: '%s' must be a string", field);
    std::vector<std::uint8_t> out;
    if (base64) {
        if (!base64_decode(std::string_view(text, n), out)) luaL_error(L, "set_mesh_data: '%s' is not valid base64", field);
    } else {
        out.assign(reinterpret_cast<const std::uint8_t*>(text), reinterpret_cast<const std::uint8_t*>(text) + n);
    }
    lua_pop(L, 1);
    return out;
}


constexpr std::size_t kMaxProceduralVertices = 4u << 20;


// Packed form: {vertices = string, indices = string, layout = "position:f32x3,...",
// index = "u32" | "u16", encoding = "base64" | "binary"}.
scene::ProceduralMesh mesh_from_packed(lua_State* L, int spec) {
    lua_getfield(L, spec, "encoding");
    const std::string encoding = luaL_optstring(L, -1, "base64");
    lua_pop(L, 1);
    if (encoding != "base64" && encoding != "binary") luaL_error(L, "set_mesh_data: encoding must be 'base64' or 'binary'");
    lua_getfield(L, spec, "layout");
    const std::string layout_text = luaL_optstring(L, -1, "position:f32x3,normal:f32x3");
    lua_pop(L, 1);
    lua_getfield(L, spec, "index");
    const std::string index_type = luaL_optstring(L, -1, "u32");
    lua_pop(L, 1);
    if (index_type != "u32" && index_type != "u16") luaL_error(L, "set_mesh_data: index must be 'u32' or 'u16'");
    std::vector<PackedAttribute> layout;
    std::string error;
    const std::uint32_t stride = parse_layout(layout_text, layout, error);
    if (stride == 0) luaL_error(L, "set_mesh_data: %s", error.c_str());
    const bool b64 = encoding == "base64";
    const std::vector<std::uint8_t> vb = field_bytes(L, spec, "vertices", b64);
    const std::vector<std::uint8_t> ib = field_bytes(L, spec, "indices", b64);
    if (vb.size() % stride != 0)
        luaL_error(L, "set_mesh_data: %d vertex bytes is not a multiple of the %d-byte layout", static_cast<int>(vb.size()), static_cast<int>(stride));
    const std::size_t n = vb.size() / stride;
    if (n > kMaxProceduralVertices) luaL_error(L, "set_mesh_data: too many vertices (%d)", static_cast<int>(n));
    const std::size_t isize = index_type == "u32" ? 4 : 2;
    if (ib.size() % isize != 0) luaL_error(L, "set_mesh_data: index bytes are not a multiple of %d", static_cast<int>(isize));
    scene::ProceduralMesh m;
    bool has_normal = false, has_uv = false, has_color = false;
    for (const auto& a : layout) {
        has_normal = has_normal || a.kind == PackedAttribute::Kind::Normal;
        has_uv = has_uv || a.kind == PackedAttribute::Kind::Uv;
        has_color = has_color || a.kind == PackedAttribute::Kind::Color;
    }
    m.positions.resize(n);
    if (has_normal) m.normals.resize(n);
    if (has_uv) m.uvs.resize(n);
    if (has_color) m.colors.resize(n);
    for (std::size_t v = 0; v < n; ++v) {
        const std::uint8_t* base = vb.data() + v * stride;
        for (const auto& a : layout) {
            const std::uint8_t* p = base + a.offset;
            switch (a.kind) {
            case PackedAttribute::Kind::Position:
                m.positions[v] = {read_component(p, a.type, 0), read_component(p, a.type, 1), read_component(p, a.type, 2)};
                break;
            case PackedAttribute::Kind::Normal:
                m.normals[v] = {read_component(p, a.type, 0), read_component(p, a.type, 1), read_component(p, a.type, 2)};
                break;
            case PackedAttribute::Kind::Uv: m.uvs[v] = {read_component(p, a.type, 0), read_component(p, a.type, 1)}; break;
            case PackedAttribute::Kind::Color:
                if (a.type == PackedAttribute::Type::U8) // bytes are taken as sRGB-encoded, alpha linear
                    m.colors[v] = std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) |
                                  (std::uint32_t{a.count == 4 ? p[3] : std::uint8_t{255}} << 24);
                else
                    m.colors[v] = pack_linear_color(read_component(p, a.type, 0), read_component(p, a.type, 1),
                                                    read_component(p, a.type, 2), a.count == 4 ? read_component(p, a.type, 3) : 1.0f);
                break;
            case PackedAttribute::Kind::Skip: break;
            }
        }
    }
    m.indices.resize(ib.size() / isize);
    for (std::size_t i = 0; i < m.indices.size(); ++i) {
        if (isize == 4) std::memcpy(&m.indices[i], ib.data() + i * 4, 4);
        else {
            std::uint16_t x;
            std::memcpy(&x, ib.data() + i * 2, 2);
            m.indices[i] = x;
        }
    }
    return m;
}


// Table form: {positions = {vector...}, indices = {integer...} (1-based), normals = {vector...}?,
// colors = {vector...}? (linear RGB), uvs = {{u, v}...}?}.
scene::ProceduralMesh mesh_from_tables(lua_State* L, int spec) {
    scene::ProceduralMesh m;
    lua_getfield(L, spec, "positions");
    luaL_checktype(L, -1, LUA_TTABLE);
    const int n = lua_objlen(L, -1);
    if (static_cast<std::size_t>(n) > kMaxProceduralVertices) luaL_error(L, "set_mesh_data: too many vertices");
    m.positions.reserve(static_cast<std::size_t>(n));
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, -1, i);
        m.positions.push_back(check_vec3(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_getfield(L, spec, "indices");
    luaL_checktype(L, -1, LUA_TTABLE);
    const int ni = lua_objlen(L, -1);
    for (int i = 1; i <= ni; ++i) {
        lua_rawgeti(L, -1, i);
        const auto idx = luaL_checkinteger(L, -1);
        if (idx < 1) luaL_error(L, "set_mesh_data: indices are 1-based (got %d)", static_cast<int>(idx));
        m.indices.push_back(static_cast<std::uint32_t>(idx - 1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_getfield(L, spec, "normals");
    if (lua_istable(L, -1)) {
        const int nn = lua_objlen(L, -1);
        for (int i = 1; i <= nn; ++i) {
            lua_rawgeti(L, -1, i);
            m.normals.push_back(check_vec3(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, spec, "colors");
    if (lua_istable(L, -1)) {
        const int nc = lua_objlen(L, -1);
        for (int i = 1; i <= nc; ++i) {
            lua_rawgeti(L, -1, i);
            const math::Vec3 c = check_vec3(L, -1);
            m.colors.push_back(pack_linear_color(c.x, c.y, c.z, 1.0f));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, spec, "uvs");
    if (lua_istable(L, -1)) {
        const int nu = lua_objlen(L, -1);
        for (int i = 1; i <= nu; ++i) {
            lua_rawgeti(L, -1, i);
            luaL_checktype(L, -1, LUA_TTABLE);
            lua_rawgeti(L, -1, 1);
            lua_rawgeti(L, -2, 2);
            m.uvs.push_back({static_cast<float>(luaL_checknumber(L, -2)), static_cast<float>(luaL_checknumber(L, -1))});
            lua_pop(L, 3);
        }
    }
    lua_pop(L, 1);
    return m;
}


// entity:set_mesh_data(spec) -> vertex count, triangle count. Builds a procedural mesh drawn by
// the entity's MeshRenderer (added when missing). Optional spec fields: scale (vector applied to
// positions; a mirroring scale also flips the winding), flip_winding (bool).
int entity_set_mesh_data(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_getfield(L, 2, "vertices");
    const bool packed = !lua_isnil(L, -1);
    lua_pop(L, 1);
    scene::ProceduralMesh m = packed ? mesh_from_packed(L, 2) : mesh_from_tables(L, 2);
    lua_getfield(L, 2, "scale");
    const math::Vec3 scale = lua_isnil(L, -1) ? math::Vec3{1, 1, 1} : check_vec3(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 2, "flip_winding");
    bool flip = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    if (scale.x != 1 || scale.y != 1 || scale.z != 1) {
        if (scale.x == 0 || scale.y == 0 || scale.z == 0) luaL_error(L, "set_mesh_data: scale components must be nonzero");
        for (auto& p : m.positions) p = {p.x * scale.x, p.y * scale.y, p.z * scale.z};
        for (auto& nrm : m.normals) {
            const math::Vec3 t{nrm.x / scale.x, nrm.y / scale.y, nrm.z / scale.z};
            const float l = math::length(t);
            nrm = l > 1e-12f ? t * (1.0f / l) : math::Vec3{0, 1, 0};
        }
        if (scale.x * scale.y * scale.z < 0) flip = !flip;
    }
    if (flip)
        for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) std::swap(m.indices[t + 1], m.indices[t + 2]);
    const auto verts = m.positions.size();
    const auto tris = m.indices.size() / 3;
    auto published = scene::publish_mesh(std::move(m));
    if (!published) luaL_error(L, "set_mesh_data: %s", published.error().message.c_str());
    auto& w = s.world();
    if (!w.get<scene::MeshRendererComponent>(e)) w.add<scene::MeshRendererComponent>(e);
    if (auto* pm = w.get<scene::ProceduralMeshComponent>(e)) pm->mesh = std::move(*published);
    else w.add<scene::ProceduralMeshComponent>(e).mesh = std::move(*published);
    lua_pushinteger(L, static_cast<int>(verts));
    lua_pushinteger(L, static_cast<int>(tris));
    return 2;
}


// entity:clear_mesh_data(): drops the procedural mesh (the MeshRenderer draws its asset again).
int entity_clear_mesh_data(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    if (s.world().get<scene::ProceduralMeshComponent>(e)) s.world().remove<scene::ProceduralMeshComponent>(e);
    return 0;
}


// entity:mesh_data() -> {vertices, triangles, min, max} | nil
int entity_mesh_data(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* pm = s.world().get<scene::ProceduralMeshComponent>(e);
    if (!pm || !pm->mesh) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, static_cast<int>(pm->mesh->positions.size()));
    lua_setfield(L, -2, "vertices");
    lua_pushinteger(L, static_cast<int>(pm->mesh->indices.size() / 3));
    lua_setfield(L, -2, "triangles");
    if (pm->mesh->bounds.valid()) {
        lua_pushvector(L, pm->mesh->bounds.min.x, pm->mesh->bounds.min.y, pm->mesh->bounds.min.z);
        lua_setfield(L, -2, "min");
        lua_pushvector(L, pm->mesh->bounds.max.x, pm->mesh->bounds.max.y, pm->mesh->bounds.max.z);
        lua_setfield(L, -2, "max");
    }
    return 1;
}


// entity:set_mesh(primitive | model_path, mesh_index?)
int entity_set_mesh(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    auto& m = check_mesh_renderer(L, s, e);
    const std::string_view name = luaL_checkstring(L, 2);
    const auto index = static_cast<std::uint32_t>(std::max(0, luaL_optinteger(L, 3, 0)));
    if (is_primitive(name)) {
        m.mesh_asset = {};
        m.primitive = std::string(name);
    } else {
        m.mesh_asset = resolve_asset(L, name, "set_mesh");
        m.primitive.clear();
    }
    m.mesh_index = index;
    return 0;
}

// entity:mesh() -> primitive name | model path, mesh index  (nil without a MeshRenderer)
int entity_mesh(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* m = s.world().get<scene::MeshRendererComponent>(e);
    if (!m) {
        lua_pushnil(L);
        return 1;
    }
    if (m->mesh_asset.is_nil()) lua_pushlstring(L, m->primitive.data(), m->primitive.size());
    else push_asset(L, m->mesh_asset);
    lua_pushinteger(L, static_cast<int>(m->mesh_index));
    return 2;
}

// entity:set_color(vector, alpha?): MeshRenderer base color (linear tint) or Sprite tint.
int entity_set_color(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const math::Vec3 c = check_vec3(L, 2);
    math::Color* target = nullptr;
    if (auto* m = s.world().get<scene::MeshRendererComponent>(e)) target = &m->base_color;
    else if (auto* sp = s.world().get<scene::SpriteComponent>(e)) target = &sp->tint;
    else luaL_error(L, "entity has no MeshRenderer or Sprite component");
    const float a = lua_isnoneornil(L, 3) ? target->a : static_cast<float>(luaL_checknumber(L, 3));
    *target = {c.x, c.y, c.z, a};
    return 0;
}

// entity:color() -> vector, alpha | nil
int entity_color(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const math::Color* c = nullptr;
    if (const auto* m = s.world().get<scene::MeshRendererComponent>(e)) c = &m->base_color;
    else if (const auto* sp = s.world().get<scene::SpriteComponent>(e)) c = &sp->tint;
    if (!c) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushvector(L, c->r, c->g, c->b);
    lua_pushnumber(L, c->a);
    return 2;
}

// entity:set_sprite(texture_path | nil): the Sprite's texture.
int entity_set_sprite(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    auto* sp = s.world().get<scene::SpriteComponent>(e);
    if (!sp) luaL_error(L, "entity has no Sprite component");
    sp->texture_asset = lua_isnoneornil(L, 2) ? Uuid{} : resolve_asset(L, luaL_checkstring(L, 2), "set_sprite");
    return 0;
}

// entity:sprite() -> texture path | nil
int entity_sprite(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const auto* sp = s.world().get<scene::SpriteComponent>(e);
    if (!sp) lua_pushnil(L);
    else push_asset(L, sp->texture_asset);
    return 1;
}

// entity:set_cast_shadows(bool)
int entity_set_cast_shadows(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    check_mesh_renderer(L, s, e).cast_shadows = lua_toboolean(L, 2) != 0;
    return 0;
}


gameplay::MaterialOverridesComponent& overrides_of(scene::Scene& s, Entity e) {
    if (auto* o = s.world().get<gameplay::MaterialOverridesComponent>(e)) return *o;
    return s.world().add<gameplay::MaterialOverridesComponent>(e);
}


// entity:set_uv_offset(vector): per-entity texture offset (x, y) on top of the material's,
// stored in MaterialOverrides (added when missing).
int entity_set_uv_offset(lua_State* L) {
    auto& s = scene_of(L);
    const Entity e = check_live(L, 1, s);
    const math::Vec3 v = check_vec3(L, 2);
    overrides_of(s, e).uv_offset = {v.x, v.y};
    return 0;
}

} // namespace victus::script::detail
