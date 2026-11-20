// SPDX-License-Identifier: Apache-2.0
// Terrain component, chunk meshes with LODs and skirts, the runtime system and height queries.
#include "victus/terrain/terrain.hpp"

#include "victus/core/log.hpp"
#include "victus/core/profiler.hpp"
#include "victus/physics/physics.hpp"
#include "victus/scene/reflect.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace victus::terrain {

using scene::Entity;
using scene::Scene;

// ---- component -----------------------------------------------------------------------------

const Uuid& TerrainComponent::layer_texture(std::uint32_t i) const noexcept {
    switch (i) {
    case 0: return layer0_texture;
    case 1: return layer1_texture;
    case 2: return layer2_texture;
    default: return layer3_texture;
    }
}
const math::Color& TerrainComponent::layer_tint(std::uint32_t i) const noexcept {
    switch (i) {
    case 0: return layer0_tint;
    case 1: return layer1_tint;
    case 2: return layer2_tint;
    default: return layer3_tint;
    }
}
float TerrainComponent::layer_tiling(std::uint32_t i) const noexcept {
    switch (i) {
    case 0: return layer0_tiling;
    case 1: return layer1_tiling;
    case 2: return layer2_tiling;
    default: return layer3_tiling;
    }
}

void register_components() {
    static std::once_flag once;
    std::call_once(once, [] {
        scene::Schema<TerrainComponent> b("Terrain", "3D");
        b.field(&TerrainComponent::data, "data", {.tooltip = "Height and splat data (.vterrain)", .hint = "terrain"})
            .field(&TerrainComponent::size, "size", {.min = 1, .max = 1e5f, .speed = 0.5f,
                                                     .tooltip = "Width (x), height of a full sample (y) and depth (z) in metres"});
        b.field(&TerrainComponent::layer0_texture, "layer0_texture", {.label = "Layer 0 Texture", .hint = "texture"})
            .field(&TerrainComponent::layer0_tint, "layer0_tint", {.label = "Layer 0 Tint"})
            .field(&TerrainComponent::layer0_tiling, "layer0_tiling", {.min = 0.01f, .max = 4096, .speed = 0.5f, .label = "Layer 0 Tiling"})
            .field(&TerrainComponent::layer1_texture, "layer1_texture", {.label = "Layer 1 Texture", .hint = "texture"})
            .field(&TerrainComponent::layer1_tint, "layer1_tint", {.label = "Layer 1 Tint"})
            .field(&TerrainComponent::layer1_tiling, "layer1_tiling", {.min = 0.01f, .max = 4096, .speed = 0.5f, .label = "Layer 1 Tiling"})
            .field(&TerrainComponent::layer2_texture, "layer2_texture", {.label = "Layer 2 Texture", .hint = "texture"})
            .field(&TerrainComponent::layer2_tint, "layer2_tint", {.label = "Layer 2 Tint"})
            .field(&TerrainComponent::layer2_tiling, "layer2_tiling", {.min = 0.01f, .max = 4096, .speed = 0.5f, .label = "Layer 2 Tiling"})
            .field(&TerrainComponent::layer3_texture, "layer3_texture", {.label = "Layer 3 Texture", .hint = "texture"})
            .field(&TerrainComponent::layer3_tint, "layer3_tint", {.label = "Layer 3 Tint"})
            .field(&TerrainComponent::layer3_tiling, "layer3_tiling", {.min = 0.01f, .max = 4096, .speed = 0.5f, .label = "Layer 3 Tiling"})
            .field(&TerrainComponent::roughness, "roughness", {.min = 0.045f, .max = 1, .speed = 0.01f})
            .field(&TerrainComponent::cast_shadows, "cast_shadows")
            .field(&TerrainComponent::collision, "collision", {.tooltip = "Static height-field collider (3D physics)"})
            .field(&TerrainComponent::friction, "friction", {.min = 0, .max = 10, .speed = 0.01f})
            .field(&TerrainComponent::collision_layer, "collision_layer", {.min = 0, .max = 15})
            .field(&TerrainComponent::collision_mask, "collision_mask", {.hint = "bitmask16"});
        const scene::ComponentSchema* fields = &b.commit();
        scene::SerializerRegistry::instance().add<TerrainComponent>(
            "Terrain", [fields](const TerrainComponent& c) { return fields->save(&c); },
            [fields](TerrainComponent& c, const json::Value& v) -> Status { return fields->load(&c, v); });

        scene::Schema<FoliageComponent> f("Foliage", "3D");
        f.field(&FoliageComponent::layer, "layer", {.min = -1, .max = 3, .tooltip = "Splat layer that drives density (-1: everywhere)"})
            .field(&FoliageComponent::min_weight, "min_weight", {.min = 0, .max = 1, .speed = 0.01f})
            .field(&FoliageComponent::density, "density", {.min = 0, .max = 100, .speed = 0.01f, .tooltip = "Candidate points per square metre"})
            .field(&FoliageComponent::min_scale, "min_scale", {.min = 0, .max = 100, .speed = 0.01f})
            .field(&FoliageComponent::max_scale, "max_scale", {.min = 0, .max = 100, .speed = 0.01f})
            .field(&FoliageComponent::align_to_normal, "align_to_normal", {.min = 0, .max = 1, .speed = 0.01f})
            .field(&FoliageComponent::random_yaw, "random_yaw")
            .field(&FoliageComponent::max_slope, "max_slope", {.min = 0, .max = 90, .speed = 0.5f, .tooltip = "Degrees"})
            .field(&FoliageComponent::min_height, "min_height", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "Normalized terrain height"})
            .field(&FoliageComponent::max_height, "max_height", {.min = 0, .max = 1, .speed = 0.01f})
            .field(&FoliageComponent::offset, "offset", {.min = -100, .max = 100, .speed = 0.01f, .tooltip = "Metres along the instance up (negative sinks)"})
            .field(&FoliageComponent::draw_distance, "draw_distance", {.min = 0, .max = 1e5f, .speed = 1})
            .field(&FoliageComponent::seed, "seed")
            .field(&FoliageComponent::max_instances, "max_instances", {.min = 1, .max = static_cast<float>(kMaxFoliageInstances)});
        const scene::ComponentSchema* ffields = &f.commit();
        scene::SerializerRegistry::instance().add<FoliageComponent>(
            "Foliage", [ffields](const FoliageComponent& c) { return ffields->save(&c); },
            [ffields](FoliageComponent& c, const json::Value& v) -> Status { return ffields->load(&c, v); });
    });
}

// ---- geometry ------------------------------------------------------------------------------

namespace {

// Value of the coarse grid (stride `st`, chunk origin ox/oz) at sample (x, z), interpolated on
// the same triangles the coarse level draws (diagonal from (x0, z0) to (x0+st, z0+st)).
template <typename Get>
float coarse_value(Get&& get, std::int32_t ox, std::int32_t oz, std::int32_t st, std::int32_t x, std::int32_t z) {
    const std::int32_t x0 = ox + ((x - ox) / st) * st, z0 = oz + ((z - oz) / st) * st;
    const std::int32_t x1 = std::min(x0 + st, ox + static_cast<std::int32_t>(kChunkQuads));
    const std::int32_t z1 = std::min(z0 + st, oz + static_cast<std::int32_t>(kChunkQuads));
    if (x0 == x1 || z0 == z1) return get(x, z);
    const float tx = static_cast<float>(x - x0) / static_cast<float>(st), tz = static_cast<float>(z - z0) / static_cast<float>(st);
    const float h00 = get(x0, z0), h10 = get(x1, z0), h01 = get(x0, z1), h11 = get(x1, z1);
    return tx >= tz ? h00 + (h10 - h00) * tx + (h11 - h10) * tz : h00 + (h01 - h00) * tz + (h11 - h01) * tx;
}

void emit_grid(std::vector<std::uint32_t>& out, std::uint32_t st) {
    constexpr std::uint32_t n = kChunkQuads + 1;
    for (std::uint32_t j = 0; j < kChunkQuads; j += st)
        for (std::uint32_t i = 0; i < kChunkQuads; i += st) {
            const std::uint32_t a = j * n + i, b = j * n + i + st, c = (j + st) * n + i + st, d = (j + st) * n + i;
            out.insert(out.end(), {a, c, b, a, d, c}); // counter-clockwise seen from +Y
        }
}

// Skirt quads below the four edges at stride `st`. Edge e's skirt vertex k sits below the grid
// vertex at position k along that edge (0: z min, 1: z max, 2: x min, 3: x max).
void emit_skirts(std::vector<std::uint32_t>& out, std::uint32_t st) {
    constexpr std::uint32_t n = kChunkQuads + 1, base = n * n;
    const auto grid = [](std::uint32_t edge, std::uint32_t k) -> std::uint32_t {
        switch (edge) {
        case 0: return k;
        case 1: return kChunkQuads * n + k;
        case 2: return k * n;
        default: return k * n + kChunkQuads;
        }
    };
    for (std::uint32_t edge = 0; edge < 4; ++edge)
        for (std::uint32_t k = 0; k < kChunkQuads; k += st) {
            const std::uint32_t p0 = grid(edge, k), p1 = grid(edge, k + st);
            const std::uint32_t q0 = base + edge * n + k, q1 = base + edge * n + k + st;
            if (edge == 0 || edge == 3) out.insert(out.end(), {p0, p1, q0, p1, q1, q0}); // faces -z / +x
            else out.insert(out.end(), {p0, q0, p1, p1, q0, q1});                         // faces +z / -x
        }
}

} // namespace

Result<std::shared_ptr<const scene::ProceduralMesh>> build_chunk(const Heightfield& hf, math::Vec3 size, std::uint32_t cx,
                                                                 std::uint32_t cz) {
    const std::uint32_t cps = hf.chunks_per_side();
    if (cx >= cps || cz >= cps) return make_error(ErrorCode::InvalidArgument, "terrain: chunk out of range");
    constexpr std::uint32_t n = kChunkQuads + 1;
    const auto ox = static_cast<std::int32_t>(cx * kChunkQuads), oz = static_cast<std::int32_t>(cz * kChunkQuads);
    const auto last = static_cast<float>(hf.resolution - 1);

    // Error of each coarser level (metres), never decreasing so coarser levels are never picked
    // earlier than finer ones: how far the full surface is from it, or, when the splat map
    // changes inside the coarse triangles (vertex colours interpolate across them), the weight
    // deviation times half the coarse cell size, so painted detail keeps its resolution until
    // the coarse cells are a few pixels on screen.
    const float cell = std::max(size.x, size.z) / last;
    const auto height = [&](std::int32_t x, std::int32_t z) { return hf.height(x, z); };
    const auto weight = [&](std::uint32_t layer) {
        return [&hf, layer](std::int32_t x, std::int32_t z) {
            const auto r = static_cast<std::int32_t>(hf.resolution) - 1;
            const std::uint32_t packed = hf.splat[hf.index(static_cast<std::uint32_t>(std::clamp(x, 0, r)),
                                                           static_cast<std::uint32_t>(std::clamp(z, 0, r)))];
            return static_cast<float>((packed >> (8 * layer)) & 0xFFu) / 255.0f;
        };
    };
    std::vector<float> errors;
    float worst = 0.0f, worst_geometry = 0.0f;
    for (std::uint32_t st = 2; st <= kChunkQuads; st *= 2) {
        const auto sti = static_cast<std::int32_t>(st);
        float e = 0.0f, w = 0.0f;
        for (std::int32_t z = oz; z <= oz + static_cast<std::int32_t>(kChunkQuads); ++z)
            for (std::int32_t x = ox; x <= ox + static_cast<std::int32_t>(kChunkQuads); ++x) {
                e = std::max(e, std::abs(hf.height(x, z) - coarse_value(height, ox, oz, sti, x, z)));
                for (std::uint32_t l = 0; l < kLayerCount; ++l) {
                    const auto get = weight(l);
                    w = std::max(w, std::abs(get(x, z) - coarse_value(get, ox, oz, sti, x, z)));
                }
            }
        worst_geometry = std::max(worst_geometry, e * size.y);
        worst = std::max({worst, e * size.y, w * cell * static_cast<float>(st) * 0.5f});
        errors.push_back(worst);
    }
    const float skirt = std::max({worst_geometry * 1.25f, size.y * 0.002f, 0.05f});

    scene::ProceduralMesh mesh;
    mesh.positions.reserve(n * n + 4 * n);
    mesh.normals.reserve(n * n + 4 * n);
    mesh.uvs.reserve(n * n + 4 * n);
    mesh.colors.reserve(n * n + 4 * n);
    const auto add = [&](std::int32_t sx, std::int32_t sz, float drop) {
        const float u = static_cast<float>(sx) / last, v = static_cast<float>(sz) / last;
        mesh.positions.push_back({(u - 0.5f) * size.x, hf.height(sx, sz) * size.y - drop, (v - 0.5f) * size.z});
        mesh.normals.push_back(sample_normal(hf, size, sx, sz));
        mesh.uvs.push_back({u, v});
        // Splat bytes go straight into the vertex colour; terrain_surface_code() undoes the
        // vertex shader's sRGB decode to get the weights back.
        mesh.colors.push_back(hf.splat[hf.index(static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>(sz))]);
    };
    for (std::uint32_t j = 0; j < n; ++j)
        for (std::uint32_t i = 0; i < n; ++i) add(ox + static_cast<std::int32_t>(i), oz + static_cast<std::int32_t>(j), 0.0f);
    const auto q = static_cast<std::int32_t>(kChunkQuads);
    for (std::uint32_t edge = 0; edge < 4; ++edge)
        for (std::int32_t k = 0; k <= q; ++k) {
            switch (edge) {
            case 0: add(ox + k, oz, skirt); break;
            case 1: add(ox + k, oz + q, skirt); break;
            case 2: add(ox, oz + k, skirt); break;
            default: add(ox + q, oz + k, skirt); break;
            }
        }
    emit_grid(mesh.indices, 1);
    emit_skirts(mesh.indices, 1);
    std::uint32_t level = 0;
    for (std::uint32_t st = 2; st <= kChunkQuads; st *= 2, ++level) {
        scene::ProceduralMesh::Lod lod;
        emit_grid(lod.indices, st);
        emit_skirts(lod.indices, st);
        lod.error = errors[level];
        mesh.lods.push_back(std::move(lod));
    }
    return scene::publish_mesh(std::move(mesh));
}

std::string_view terrain_surface_code() noexcept {
    return R"glsl(
// Built-in terrain surface (engine/terrain): blends four layers by the splat weights stored in
// the vertex colours. params[i] = layer i tint (rgb) and tiling (w, 0 = untextured).
//
// Layer texture alpha is an optional height map (ADR-0121): 1 everywhere (an opaque texture)
// reproduces the plain weighted blend exactly. Lower alpha marks crevices, which
//   - lose the blend against taller neighbours (height-based blending: stones and leaves poke
//     through mud instead of cross-fading),
//   - bend the shading normal (bump from screen-space derivatives of the blended height,
//     Mikkelsen 2010, so no tangents or extra texture fetches are needed),
//   - get a little cavity occlusion and hold moisture (slightly smoother).
// The bump depth is 4 cm times the material normal scale (TerrainComponent roughness keeps its
// meaning as the base roughness).
vec3 terrain_encode_srgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}
vec4 terrain_layer(sampler2D t, vec4 p, vec2 uv, vec2 dx, vec2 dy) {
    if (p.w <= 0.0) return vec4(p.rgb, 1.0);
    vec4 c = textureGrad(t, uv * p.w, dx * p.w, dy * p.w);
    return vec4(c.rgb * p.rgb, c.a);
}
void surface(SurfaceInput i, inout Surface s) {
    vec4 w = vec4(clamp(terrain_encode_srgb(i.vertex_color.rgb), 0.0, 1.0), i.vertex_color.a);
    w /= max(w.x + w.y + w.z + w.w, 1e-4);
    // gradients are taken here, in uniform control flow, and passed into the branches below
    vec2 dx = dFdx(i.mesh_uv), dy = dFdy(i.mesh_uv);
    vec4 l0 = vec4(0.0), l1 = vec4(0.0), l2 = vec4(0.0), l3 = vec4(0.0);
    if (w.x > 0.002) l0 = terrain_layer(u_base_color, i.params[0], i.mesh_uv, dx, dy);
    if (w.y > 0.002) l1 = terrain_layer(u_metal_rough, i.params[1], i.mesh_uv, dx, dy);
    if (w.z > 0.002) l2 = terrain_layer(u_normal_map, i.params[2], i.mesh_uv, dx, dy);
    if (w.w > 0.002) l3 = terrain_layer(u_emissive, i.params[3], i.mesh_uv, dx, dy);
    vec4 h = vec4(l0.a, l1.a, l2.a, l3.a);
    // height blend: exp2(k (h - hmax)) is 1 for every layer when the heights are equal
    float hmax = max(max(w.x > 0.002 ? h.x : 0.0, w.y > 0.002 ? h.y : 0.0),
                     max(w.z > 0.002 ? h.z : 0.0, w.w > 0.002 ? h.w : 0.0));
    vec4 hw = w * exp2(12.0 * (h - hmax)) * step(vec4(0.002), w);
    hw /= max(hw.x + hw.y + hw.z + hw.w, 1e-4);
    vec3 c = hw.x * l0.rgb + hw.y * l1.rgb + hw.z * l2.rgb + hw.w * l3.rgb;
    float height = dot(hw, h);
    // bump from the blended height (world metres), surface gradient form
    vec3 N = normalize(i.vertex_normal);
    float amp = 0.04 * material.params.z;
    vec3 sx = dFdx(i.world_position), sy = dFdy(i.world_position);
    float hx = dFdx(height) * amp, hy = dFdy(height) * amp;
    vec3 r1 = cross(sy, N), r2 = cross(N, sx);
    float det = dot(sx, r1);
    vec3 grad = sign(det) * (hx * r1 + hy * r2);
    vec3 bumped = abs(det) * N - grad;
    float bl = dot(bumped, bumped);
    s.normal = bl > 1e-20 ? normalize(bumped) : N;
    s.base_color = c;
    s.alpha = 1.0;
    s.metallic = 0.0;
    float cavity = 1.0 - height; // 0 on an opaque (legacy) texture
    s.roughness = clamp(material.params.y * (1.0 - 0.25 * cavity), 0.045, 1.0);
    s.emissive = vec3(0.0);
    s.occlusion = 1.0 - 0.6 * cavity * cavity;
}
)glsl";
}

// ---- runtime -------------------------------------------------------------------------------

TerrainSystem::TerrainSystem(TerrainProvider provider) : provider_(std::move(provider)) { register_components(); }

void TerrainSystem::add_data(const Uuid& id, std::shared_ptr<Heightfield> data) { added_[id] = std::move(data); }

void TerrainSystem::clear_cache() { cache_.clear(); }

std::shared_ptr<Heightfield> TerrainSystem::data(const Uuid& id) {
    if (id.is_nil()) return nullptr;
    if (const auto it = added_.find(id); it != added_.end()) return it->second;
    if (const auto it = cache_.find(id); it != cache_.end()) return it->second;
    std::shared_ptr<Heightfield> d = provider_ ? provider_(id) : nullptr;
    cache_[id] = d; // misses are cached too until clear_cache()
    return d;
}

void TerrainSystem::update(Scene& scene) {
    VICTUS_PROFILE_SCOPE("terrain.update");
    stats_.terrains = 0;
    stats_.missing_data = 0;
    stats_.chunks = 0;
    auto& world = scene.world();
    std::vector<Entity> terrains;
    world.each<TerrainComponent>([&](Entity e, TerrainComponent&) { terrains.push_back(e); });
    for (const Entity e : terrains) {
        ++stats_.terrains;
        if (!world.get<TerrainRuntime>(e)) world.add<TerrainRuntime>(e);
        const TerrainComponent comp = *world.get<TerrainComponent>(e); // copy: adding components may move storage
        std::shared_ptr<Heightfield> hf = data(comp.data);
        if (hf && (hf->empty() || hf->heights.size() != static_cast<std::size_t>(hf->resolution) * hf->resolution ||
                   hf->splat.size() != hf->heights.size()))
            hf = nullptr;
        const math::Vec3 size{std::max(comp.size.x, 0.01f), std::max(comp.size.y, 0.0f), std::max(comp.size.z, 0.01f)};
        auto* rt = world.get<TerrainRuntime>(e);
        if (!hf) {
            ++stats_.missing_data;
            rt->data.reset();
            rt->built_data = nullptr;
            rt->chunks.clear();
            rt->chunk_built.clear();
            rt->chunks_per_side = 0;
            if (world.get<physics::StaticHeightfieldComponent>(e)) world.remove<physics::StaticHeightfieldComponent>(e);
            continue;
        }
        const std::uint32_t cps = hf->chunks_per_side();
        if (hf->chunk_revisions.size() != static_cast<std::size_t>(cps) * cps) hf->touch_all();
        const bool full = rt->built_data != hf.get() || !(rt->built_size == size) || rt->chunks_per_side != cps;
        if (full) {
            rt->chunks.assign(static_cast<std::size_t>(cps) * cps, nullptr);
            rt->chunk_built.assign(rt->chunks.size(), ~0ull);
            rt->chunks_per_side = cps;
            rt->collision_revision = ~0ull;
        }
        rt->data = hf;
        rt->resolved = comp.data;
        rt->built_data = hf.get();
        rt->built_size = size;
        for (std::size_t c = 0; c < rt->chunks.size(); ++c) {
            if (rt->chunk_built[c] == hf->chunk_revisions[c] && rt->chunks[c]) continue;
            auto mesh = build_chunk(*hf, size, static_cast<std::uint32_t>(c % cps), static_cast<std::uint32_t>(c / cps));
            if (!mesh) {
                VICTUS_LOG_ERROR("terrain", "chunk {}: {}", c, mesh.error().message);
                continue;
            }
            rt->chunks[c] = std::move(*mesh);
            rt->chunk_built[c] = hf->chunk_revisions[c];
            ++stats_.chunk_rebuilds;
        }
        stats_.chunks += static_cast<std::uint32_t>(rt->chunks.size());

        // Collision: rebuild the height field when the data or the settings changed.
        auto* shape = world.get<physics::StaticHeightfieldComponent>(e);
        if (!comp.collision) {
            if (shape) world.remove<physics::StaticHeightfieldComponent>(e);
            world.get<TerrainRuntime>(e)->collision_revision = ~0ull;
            continue;
        }
        const bool settings_same = shape && shape->friction == comp.friction && shape->layer == comp.collision_layer &&
                                   shape->mask == comp.collision_mask;
        if (shape && settings_same && rt->collision_revision == hf->revision) continue;
        // An edit (same data, size and resolution): copy only the samples of the chunks touched
        // since the collider was last brought up to date and hand physics that rectangle.
        if (shape && settings_same && !full && rt->collision_revision != ~0ull && rt->collision_heights &&
            shape->heights == rt->collision_heights && shape->resolution == hf->resolution &&
            hf->chunk_revisions.size() == static_cast<std::size_t>(cps) * cps) {
            physics::HeightfieldRect rect;
            const std::uint32_t last = hf->resolution - 1;
            for (std::uint32_t cz = 0; cz < cps; ++cz)
                for (std::uint32_t cx = 0; cx < cps; ++cx) {
                    if (hf->chunk_revisions[static_cast<std::size_t>(cz) * cps + cx] <= rt->collision_revision) continue;
                    rect.include({cx * kChunkQuads, cz * kChunkQuads, std::min(last, (cx + 1) * kChunkQuads),
                                  std::min(last, (cz + 1) * kChunkQuads)});
                }
            if (rect.empty()) rect = {0, 0, last, last}; // revision moved without a chunk: be safe
            auto& heights = *rt->collision_heights;
            for (std::uint32_t z = rect.z0; z <= rect.z1; ++z)
                for (std::uint32_t x = rect.x0; x <= rect.x1; ++x) heights[hf->index(x, z)] = hf->heights[hf->index(x, z)] * size.y;
            shape->dirty.include(rect);
            ++shape->height_revision;
            rt->collision_revision = hf->revision;
            ++stats_.collision_updates;
            continue;
        }
        physics::StaticHeightfieldComponent out;
        out.resolution = hf->resolution;
        auto heights = std::make_shared<std::vector<float>>(hf->heights.size());
        for (std::size_t i = 0; i < hf->heights.size(); ++i) (*heights)[i] = hf->heights[i] * size.y;
        rt->collision_heights = heights;
        out.heights = std::move(heights);
        out.height_range = {0.0f, size.y}; // normalized heights 0..1 scaled by size.y
        const auto last = static_cast<float>(hf->resolution - 1);
        out.offset = {-size.x * 0.5f, 0.0f, -size.z * 0.5f};
        out.spacing = {size.x / last, size.z / last};
        out.friction = comp.friction;
        out.layer = comp.collision_layer;
        out.mask = comp.collision_mask;
        out.revision = (shape ? shape->revision : 0) + 1;
        const std::uint64_t revision = hf->revision;
        if (shape) *shape = std::move(out);
        else world.add<physics::StaticHeightfieldComponent>(e, std::move(out));
        world.get<TerrainRuntime>(e)->collision_revision = revision;
        ++stats_.collision_rebuilds;
    }
    update_foliage(scene);
}

void add_terrain_systems(gameplay::Simulation& sim, TerrainSystem& system) {
    TerrainSystem* sys = &system;
    sim.add_system("terrain.pre", gameplay::Phase::PreUpdate, [sys](Scene& scene, const gameplay::FrameTime&) { sys->update(scene); });
    sim.add_system("terrain.post", gameplay::Phase::PostUpdate, [sys](Scene& scene, const gameplay::FrameTime&) { sys->update(scene); });
}

std::optional<float> height_at(Scene& scene, Entity entity, float x, float z) {
    auto& world = scene.world();
    const auto* comp = world.get<TerrainComponent>(entity);
    const auto* rt = world.get<TerrainRuntime>(entity);
    const auto* wt = world.get<scene::WorldTransform>(entity);
    if (!comp || !rt || !rt->data || !wt) return std::nullopt;
    // Rigid placement: translation and rotation of the world transform, scale ignored.
    const math::Mat4 m = rigid_matrix(wt->matrix);
    const math::Mat4 inv = math::inverse(m);
    // Cast along the terrain's local up through the world point: intersect the vertical line.
    const math::Vec3 local = inv.transform_point({x, 0.0f, z});
    const math::Vec3 up = (inv * math::Vec4(0, 1, 0, 0)).xyz();
    if (std::abs(up.y) < 1e-4f) return std::nullopt; // terrain rotated onto its side
    // Points on the vertical world line: local + up * t. Solve for the local x/z of the terrain
    // surface iteratively (exact for upright terrains, converges for tilted ones).
    float t = 0.0f;
    std::optional<float> h;
    for (int it = 0; it < 4; ++it) {
        const math::Vec3 p = local + up * t;
        h = local_height(*rt->data, rt->built_size, p.x, p.z);
        if (!h) return std::nullopt;
        t = (*h - local.y) / up.y;
    }
    const math::Vec3 p = local + up * t;
    return m.transform_point({p.x, *h, p.z}).y;
}

std::optional<float> height_at(Scene& scene, float x, float z) {
    std::optional<float> out;
    scene.world().each<TerrainComponent>([&](Entity e, TerrainComponent&) {
        if (!out) out = height_at(scene, e, x, z);
    });
    return out;
}

} // namespace victus::terrain
