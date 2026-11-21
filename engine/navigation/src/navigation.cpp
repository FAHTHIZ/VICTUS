// SPDX-License-Identifier: Apache-2.0
#include "victus/navigation/navigation.hpp"

#include "victus/core/log.hpp"
#include "victus/core/profiler.hpp"
#include "victus/physics/physics.hpp"
#include "victus/scene/reflect.hpp"

#include <DetourCommon.h>
#include <DetourCrowd.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <future>
#include <string>
#include <mutex>
#include <numbers>
#include <random>
#include <unordered_map>

namespace victus::navigation {

using scene::Scene;

namespace {

constexpr int kMaxPathPolys = 512;
constexpr float kPi = std::numbers::pi_v<float>;
constexpr unsigned short kWalkFlag = 0x01;

// ---- geometry collection ---------------------------------------------------------------------------

math::Vec3 vec_at(const std::vector<float>& v, int i) {
    const auto k = static_cast<std::size_t>(i) * 3;
    return {v[k], v[k + 1], v[k + 2]};
}

struct Geometry {
    std::vector<float> verts;
    std::vector<int> tris;
    float bmin[3]{FLT_MAX, FLT_MAX, FLT_MAX};
    float bmax[3]{-FLT_MAX, -FLT_MAX, -FLT_MAX};

    int vertex(math::Vec3 p) {
        verts.insert(verts.end(), {p.x, p.y, p.z});
        bmin[0] = std::min(bmin[0], p.x), bmin[1] = std::min(bmin[1], p.y), bmin[2] = std::min(bmin[2], p.z);
        bmax[0] = std::max(bmax[0], p.x), bmax[1] = std::max(bmax[1], p.y), bmax[2] = std::max(bmax[2], p.z);
        return static_cast<int>(verts.size() / 3 - 1);
    }
    // World-space triangle wound so its normal points away from the (convex) shape's center;
    // Recast treats triangles whose normal points up as walkable.
    void triangle(math::Vec3 a, math::Vec3 b, math::Vec3 c, math::Vec3 center) {
        const math::Vec3 n = math::cross(b - a, c - a);
        if (math::length(n) < 1e-9f) return;
        const float side = math::dot(n, (a + b + c) * (1.0f / 3.0f) - center);
        const bool flip = std::fabs(side) < 1e-7f ? n.y < 0 : side < 0;
        if (flip) std::swap(b, c);
        tris.insert(tris.end(), {vertex(a), vertex(b), vertex(c)});
    }
    [[nodiscard]] int triangle_count() const { return static_cast<int>(tris.size() / 3); }

    // One contributing collider or height field: its triangles, bounds and a content hash, so a
    // tiled build can tell which tiles a change touches.
    struct Source {
        std::uint64_t hash{0};
        float bmin[3]{FLT_MAX, FLT_MAX, FLT_MAX};
        float bmax[3]{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    };
    std::vector<Source> sources;
    std::size_t source_vert_begin{0};
    void begin_source() { source_vert_begin = verts.size(); }
    void end_source() {
        if (verts.size() == source_vert_begin) return;
        Source src;
        std::uint64_t h = 1469598103934665603ull;
        for (std::size_t i = source_vert_begin; i < verts.size(); ++i) {
            std::uint32_t bits;
            std::memcpy(&bits, &verts[i], sizeof bits);
            for (int k = 0; k < 4; ++k) h = (h ^ ((bits >> (8 * k)) & 0xffu)) * 1099511628211ull;
            const int axis = static_cast<int>((i - source_vert_begin) % 3);
            src.bmin[axis] = std::min(src.bmin[axis], verts[i]);
            src.bmax[axis] = std::max(src.bmax[axis], verts[i]);
        }
        src.hash = h;
        sources.push_back(src);
    }
};

// Revolves a (radius, y) profile around local Y (spheres, capsules, cylinders).
void lathe(Geometry& g, const math::Mat4& m, math::Vec3 offset, const std::vector<math::Vec2>& profile, int segments) {
    const math::Vec3 center = m.transform_point(offset);
    auto point = [&](math::Vec2 p, int s) {
        const float a = 2.0f * kPi * static_cast<float>(s) / static_cast<float>(segments);
        return m.transform_point(offset + math::Vec3{p.x * std::cos(a), p.y, p.x * std::sin(a)});
    };
    for (std::size_t i = 0; i + 1 < profile.size(); ++i) {
        for (int s = 0; s < segments; ++s) {
            const math::Vec3 a = point(profile[i], s), b = point(profile[i], s + 1);
            const math::Vec3 c = point(profile[i + 1], s + 1), d = point(profile[i + 1], s);
            if (profile[i].x > 1e-6f) g.triangle(a, b, c, center);
            if (profile[i + 1].x > 1e-6f) g.triangle(a, c, d, center);
        }
    }
}

// Convex and mesh colliders contribute their source triangles with the authored winding (glTF:
// counter-clockwise front faces, so floors face up); a mirroring transform flips them back.
void add_mesh(Geometry& g, const math::Mat4& m, math::Vec3 offset, const physics::CollisionMesh& mesh) {
    const math::Vec3 x = (m * math::Vec4(1, 0, 0, 0)).xyz(), y = (m * math::Vec4(0, 1, 0, 0)).xyz(), z = (m * math::Vec4(0, 0, 1, 0)).xyz();
    const bool mirrored = math::dot(math::cross(x, y), z) < 0;
    const int first = static_cast<int>(g.verts.size() / 3);
    for (const math::Vec3& p : mesh.positions) (void)g.vertex(m.transform_point(p + offset));
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const int a = first + static_cast<int>(mesh.indices[i]);
        int b = first + static_cast<int>(mesh.indices[i + 1]), c = first + static_cast<int>(mesh.indices[i + 2]);
        if (mirrored) std::swap(b, c);
        g.tris.insert(g.tris.end(), {a, b, c});
    }
}

void add_collider(Geometry& g, const math::Mat4& m, const physics::ColliderComponent& c, const physics::CollisionMesh* mesh) {
    using Shape = physics::ColliderComponent::Shape;
    switch (c.shape) {
    case Shape::Box: {
        const math::Vec3 h = c.size * 0.5f;
        math::Vec3 p[8];
        for (int i = 0; i < 8; ++i)
            p[i] = m.transform_point(c.offset + math::Vec3{(i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z});
        const math::Vec3 center = m.transform_point(c.offset);
        static constexpr int kFaces[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
        for (const auto& f : kFaces) {
            g.triangle(p[f[0]], p[f[1]], p[f[2]], center);
            g.triangle(p[f[0]], p[f[2]], p[f[3]], center);
        }
        break;
    }
    case Shape::Sphere: {
        std::vector<math::Vec2> profile;
        for (int i = 0; i <= 8; ++i) {
            const float a = -kPi * 0.5f + kPi * static_cast<float>(i) / 8.0f;
            profile.push_back({c.radius * std::cos(a), c.radius * std::sin(a)});
        }
        lathe(g, m, c.offset, profile, 16);
        break;
    }
    case Shape::Capsule: {
        const float half = std::max(0.0f, c.height * 0.5f - c.radius);
        std::vector<math::Vec2> profile;
        for (int i = 0; i <= 4; ++i) {
            const float a = -kPi * 0.5f + kPi * 0.5f * static_cast<float>(i) / 4.0f;
            profile.push_back({c.radius * std::cos(a), -half + c.radius * std::sin(a)});
        }
        for (int i = 0; i <= 4; ++i) {
            const float a = kPi * 0.5f * static_cast<float>(i) / 4.0f;
            profile.push_back({c.radius * std::cos(a), half + c.radius * std::sin(a)});
        }
        lathe(g, m, c.offset, profile, 16);
        break;
    }
    case Shape::Cylinder: {
        const float h = c.height * 0.5f;
        lathe(g, m, c.offset, {{0, -h}, {c.radius, -h}, {c.radius, h}, {0, h}}, 16);
        break;
    }
    case Shape::Convex:
    case Shape::Mesh:
        if (mesh) add_mesh(g, m, c.offset, *mesh);
        break;
    }
}

[[nodiscard]] bool uses_mesh(const physics::ColliderComponent& c) {
    return c.shape == physics::ColliderComponent::Shape::Convex || c.shape == physics::ColliderComponent::Shape::Mesh;
}

const NavMeshSurfaceComponent* surface_of(Scene& scene) {
    const NavMeshSurfaceComponent* found = nullptr;
    scene.world().each<NavMeshSurfaceComponent>([&](Entity, NavMeshSurfaceComponent& s) {
        if (!found) found = &s;
    });
    return found;
}

// Visits every collider that contributes navmesh geometry: static ones as their shape, kinematic
// ones (moving platforms, spinning obstacles) as an upright bounding cylinder so rotating about Y
// does not change the navmesh. Dynamic bodies, triggers and agents are ignored. Convex/mesh
// colliders get their geometry from the bound physics world's mesh cache; without a world (or
// geometry) they are skipped.
template <typename Fn>
void each_static_collider(Scene& scene, std::uint32_t layer_mask, Fn&& fn) {
    ecs::World& w = scene.world();
    physics::PhysicsWorld* pw = physics::world_of(scene);
    w.each<physics::ColliderComponent>([&](Entity e, physics::ColliderComponent& c) {
        if (c.is_trigger) return;
        if (c.layer < 32 && !(layer_mask & (1u << c.layer))) return;
        bool kinematic = false;
        if (const auto* rb = w.get<physics::RigidBodyComponent>(e)) {
            if (rb->type == physics::BodyType::Dynamic) return;
            kinematic = rb->type == physics::BodyType::Kinematic;
        }
        if (w.has<NavAgentComponent>(e) || w.has<physics::CharacterControllerComponent>(e)) return;
        std::shared_ptr<const physics::CollisionMesh> mesh;
        if (uses_mesh(c)) {
            mesh = pw ? pw->mesh_cache().resolve(scene, e, c) : nullptr;
            if (!mesh || !mesh->bounds.valid()) return;
        }
        fn(e, c, kinematic, mesh.get());
    });
}

// Static height fields (terrains) placed by the translation and rotation of their world
// transform, like the physics body.
math::Mat4 rigid(const math::Mat4& world) {
    math::Vec3 t, sc;
    math::Quat q;
    math::decompose(world, t, q, sc);
    return math::Mat4::trs(t, q, {1, 1, 1});
}

// Large fields are sampled with a stride so at most kMaxFieldQuads quads per side reach Recast
// (its voxel resolution is coarser than that anyway).
constexpr std::uint32_t kMaxFieldQuads = 512;

template <typename Fn>
void each_static_heightfield(Scene& scene, std::uint32_t layer_mask, Fn&& fn) {
    scene.world().each<physics::StaticHeightfieldComponent>([&](Entity e, physics::StaticHeightfieldComponent& c) {
        if (c.resolution < 2 || !c.heights || c.heights->size() < static_cast<std::size_t>(c.resolution) * c.resolution) return;
        if (c.layer < 32 && !(layer_mask & (1u << c.layer))) return;
        fn(e, c);
    });
}

void add_heightfield(Geometry& g, const math::Mat4& m, const physics::StaticHeightfieldComponent& c) {
    const std::uint32_t quads = c.resolution - 1;
    const std::uint32_t stride = std::max(1u, (quads + kMaxFieldQuads - 1) / kMaxFieldQuads);
    std::vector<std::uint32_t> xs;
    for (std::uint32_t i = 0; i < quads; i += stride) xs.push_back(i);
    xs.push_back(quads);
    const auto& h = *c.heights;
    const math::Vec3 up = (m * math::Vec4(0, 1, 0, 0)).xyz();
    auto point = [&](std::uint32_t x, std::uint32_t z) {
        return m.transform_point(c.offset + math::Vec3{static_cast<float>(x) * c.spacing.x,
                                                       h[static_cast<std::size_t>(z) * c.resolution + x],
                                                       static_cast<float>(z) * c.spacing.y});
    };
    const std::size_t first = g.verts.size() / 3;
    const std::size_t n = xs.size();
    for (const std::uint32_t z : xs)
        for (const std::uint32_t x : xs) (void)g.vertex(point(x, z));
    for (std::size_t j = 0; j + 1 < n; ++j) {
        for (std::size_t i = 0; i + 1 < n; ++i) {
            const int a = static_cast<int>(first + j * n + i), b = a + 1;
            const int cc = static_cast<int>(first + (j + 1) * n + i), d = cc + 1;
            // Wind each triangle so its normal points along the field's up axis.
            auto tri = [&](int i0, int i1, int i2) {
                const auto v = [&](int k) { return vec_at(g.verts, k); };
                if (math::dot(math::cross(v(i1) - v(i0), v(i2) - v(i0)), up) < 0) std::swap(i1, i2);
                g.tris.insert(g.tris.end(), {i0, i1, i2});
            };
            tri(a, cc, b);
            tri(b, cc, d);
        }
    }
}

struct Bound {
    math::Vec3 center; // on the vertical axis
    float radius{0}, y_min{0}, y_max{0};
};

// Rotation-about-Y invariant bound of a collider: vertical cylinder around the shape's center.
Bound upright_bound(const math::Mat4& m, const physics::ColliderComponent& c, const physics::CollisionMesh* mesh) {
    using Shape = physics::ColliderComponent::Shape;
    math::Vec3 h = c.shape == Shape::Box      ? c.size * 0.5f
                   : c.shape == Shape::Sphere ? math::Vec3{c.radius, c.radius, c.radius}
                                              : math::Vec3{c.radius, std::max(c.height * 0.5f, c.radius), c.radius};
    math::Vec3 centre = c.offset;
    if (mesh && uses_mesh(c)) {
        h = mesh->bounds.extents();
        centre = c.offset + mesh->bounds.center();
    }
    Bound b;
    b.center = m.transform_point(centre);
    b.y_min = FLT_MAX, b.y_max = -FLT_MAX;
    for (int i = 0; i < 8; ++i) {
        const math::Vec3 p = m.transform_point(centre + math::Vec3{(i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z});
        b.radius = std::max(b.radius, std::hypot(p.x - b.center.x, p.z - b.center.z));
        b.y_min = std::min(b.y_min, p.y), b.y_max = std::max(b.y_max, p.y);
    }
    // Quantize (2 cm) so tiny numeric drift from a spinning transform does not trigger rebuilds.
    auto q = [](float v) { return std::round(v * 50.0f) / 50.0f; };
    b.center = {q(b.center.x), q(b.center.y), q(b.center.z)};
    b.radius = q(b.radius + 0.01f), b.y_min = q(b.y_min), b.y_max = q(b.y_max);
    return b;
}

void add_bound(Geometry& g, const Bound& b) {
    const math::Mat4 m = math::Mat4::trs({b.center.x, 0, b.center.z}, {}, {1, 1, 1});
    lathe(g, m, {}, {{0, b.y_min}, {b.radius, b.y_min}, {b.radius, b.y_max}, {0, b.y_max}}, 16);
}

struct Hasher {
    std::uint64_t h{1469598103934665603ull};
    void bytes(const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    }
    template <typename T>
    void value(const T& v) {
        bytes(&v, sizeof(T));
    }
};

std::uint64_t geometry_signature(Scene& scene, const NavMeshSurfaceComponent& s) {
    Hasher hs;
    hs.value(s.agent_radius), hs.value(s.agent_height), hs.value(s.max_climb), hs.value(s.max_slope);
    hs.value(s.cell_size), hs.value(s.cell_height), hs.value(s.layer_mask);
    each_static_collider(scene, s.layer_mask,
                         [&](Entity e, const physics::ColliderComponent& c, bool kinematic, const physics::CollisionMesh* mesh) {
        hs.value(e.bits());
        hs.value(kinematic);
        const math::Mat4 m = scene.world_matrix(e);
        if (kinematic) {
            const Bound b = upright_bound(m, c, mesh);
            hs.value(b.center), hs.value(b.radius), hs.value(b.y_min), hs.value(b.y_max);
            return;
        }
        hs.value(c.shape), hs.value(c.size), hs.value(c.radius), hs.value(c.height), hs.value(c.offset);
        hs.value(mesh); // cached geometry: a reloaded mesh is a new object
        hs.bytes(&m, sizeof(m));
    });
    each_static_heightfield(scene, s.layer_mask, [&](Entity e, const physics::StaticHeightfieldComponent& c) {
        hs.value(e.bits());
        hs.value(c.resolution), hs.value(c.revision), hs.value(c.offset), hs.value(c.spacing);
        hs.value(c.heights.get());
        const math::Mat4 m = rigid(scene.world_matrix(e));
        hs.bytes(&m, sizeof(m));
    });
    return hs.h;
}

math::Vec3 vec(const float* p) { return {p[0], p[1], p[2]}; }

float shortest_angle(float from, float to) {
    float d = std::fmod(to - from, 2.0f * kPi);
    if (d > kPi) d -= 2.0f * kPi;
    if (d < -kPi) d += 2.0f * kPi;
    return d;
}

std::mt19937* g_random = nullptr;
float random_unit() { return std::uniform_real_distribution<float>(0.0f, 0.99999f)(*g_random); }

// Voxel columns above which the automatic mode switches to a tiled navmesh.
constexpr double kAutoTileCells = 1024.0 * 1024.0;
constexpr int kAutoTileCellsPerSide = 256;
constexpr int kMaxTiles = 4096; // 12 tile bits; 10+ poly bits keep 32-bit poly refs valid

[[nodiscard]] bool wants_tiles(const Geometry& geo, const NavMeshSurfaceComponent& s) {
    if (geo.tris.empty()) return false;
    if (s.tile_size > 0.0f) return true;
    const double cs = std::max(s.cell_size, 0.01f);
    return (geo.bmax[0] - geo.bmin[0]) / cs * ((geo.bmax[2] - geo.bmin[2]) / cs) > kAutoTileCells;
}

rcConfig make_grid_config(const Geometry& geo, const NavMeshSurfaceComponent& s, bool tiled = false) {
    rcConfig cfg{};
    cfg.cs = std::max(s.cell_size, 0.01f);
    cfg.ch = std::max(s.cell_height, 0.01f);
    // Keep the voxel grid bounded on very large single-tile levels; tiled meshes keep full detail
    // (the tile layout bounds the work per tile instead).
    const float extent_x = geo.bmax[0] - geo.bmin[0], extent_z = geo.bmax[2] - geo.bmin[2];
    const float max_cells = tiled ? 65536.0f * 65536.0f : 4096.0f * 4096.0f;
    if (extent_x / cfg.cs * (extent_z / cfg.cs) > max_cells) cfg.cs = std::sqrt(extent_x * extent_z / max_cells);
    cfg.walkableSlopeAngle = std::clamp(s.max_slope, 0.0f, 89.0f);
    cfg.walkableHeight = static_cast<int>(std::ceil(std::max(s.agent_height, cfg.ch) / cfg.ch));
    cfg.walkableClimb = static_cast<int>(std::floor(std::max(s.max_climb, 0.0f) / cfg.ch));
    cfg.walkableRadius = static_cast<int>(std::ceil(std::max(s.agent_radius, 0.0f) / cfg.cs));
    cfg.maxEdgeLen = static_cast<int>(12.0f / cfg.cs);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea = 8 * 8;
    cfg.mergeRegionArea = 20 * 20;
    cfg.maxVertsPerPoly = 6;
    cfg.detailSampleDist = cfg.cs * 6.0f;
    cfg.detailSampleMaxError = cfg.ch * 1.0f;
    rcVcopy(cfg.bmin, geo.bmin);
    rcVcopy(cfg.bmax, geo.bmax);
    cfg.bmax[1] += s.agent_height; // headroom above the highest surface
    if (geo.tris.empty()) return cfg;
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);
    return cfg;
}

// Tile grid of a tiled navmesh. Two builds with equal layouts can swap individual tiles.
struct TileLayout {
    bool tiled{false};
    float orig[3]{0, 0, 0};
    float tile_world{0};  // tile side in metres
    int tile_cells{0};    // tile side in voxels
    int tw{0}, th{0};     // tiles along X and Z
    float cs{0};
    int border{0};        // voxels of overlap rasterized around each tile
    int max_polys{0};
    std::uint64_t settings_hash{0};
    [[nodiscard]] bool same_grid(const TileLayout& o) const noexcept {
        return tiled && o.tiled && orig[0] == o.orig[0] && orig[2] == o.orig[2] && tile_world == o.tile_world &&
               tile_cells == o.tile_cells && tw == o.tw && th == o.th && cs == o.cs && settings_hash == o.settings_hash;
    }
    [[nodiscard]] int count() const noexcept { return tw * th; }
};

std::uint64_t surface_hash(const NavMeshSurfaceComponent& s) {
    Hasher hs;
    hs.value(s.agent_radius), hs.value(s.agent_height), hs.value(s.max_climb), hs.value(s.max_slope);
    hs.value(s.cell_size), hs.value(s.cell_height), hs.value(s.tile_size);
    return hs.h;
}

TileLayout make_tile_layout(const Geometry& geo, const NavMeshSurfaceComponent& s, const rcConfig& cfg) {
    TileLayout L;
    L.tiled = true;
    L.cs = cfg.cs;
    L.border = cfg.walkableRadius + 3;
    L.settings_hash = surface_hash(s);
    int cells = s.tile_size > 0.0f ? static_cast<int>(std::ceil(s.tile_size / cfg.cs)) : kAutoTileCellsPerSide;
    cells = std::clamp(cells, 32, 1024);
    for (;;) {
        L.tile_cells = cells;
        L.tile_world = static_cast<float>(cells) * cfg.cs;
        // Snap the origin to the tile grid so geometry growing at one edge keeps the other tiles.
        L.orig[0] = std::floor(geo.bmin[0] / L.tile_world) * L.tile_world;
        L.orig[1] = cfg.bmin[1];
        L.orig[2] = std::floor(geo.bmin[2] / L.tile_world) * L.tile_world;
        L.tw = std::max(1, static_cast<int>(std::ceil((geo.bmax[0] - L.orig[0]) / L.tile_world)));
        L.th = std::max(1, static_cast<int>(std::ceil((geo.bmax[2] - L.orig[2]) / L.tile_world)));
        if (L.count() <= kMaxTiles || cells >= 4096) break;
        cells *= 2;
    }
    const int tile_bits = static_cast<int>(dtIlog2(dtNextPow2(static_cast<unsigned>(std::max(L.count(), 1)))));
    L.max_polys = 1 << std::min(22 - tile_bits, 16);
    return L;
}

// Tile (x, z) bounds in world XZ including the rasterization border.
void tile_bounds(const TileLayout& L, int tx, int tz, float bmin[2], float bmax[2]) {
    const float pad = static_cast<float>(L.border) * L.cs;
    bmin[0] = L.orig[0] + static_cast<float>(tx) * L.tile_world - pad;
    bmin[1] = L.orig[2] + static_cast<float>(tz) * L.tile_world - pad;
    bmax[0] = L.orig[0] + static_cast<float>(tx + 1) * L.tile_world + pad;
    bmax[1] = L.orig[2] + static_cast<float>(tz + 1) * L.tile_world + pad;
}

// Inclusive tile index range touched by an XZ box (with the border), clamped to the grid.
void tile_range(const TileLayout& L, float minx, float minz, float maxx, float maxz, int& x0, int& z0, int& x1, int& z1) {
    const float pad = static_cast<float>(L.border) * L.cs;
    x0 = std::clamp(static_cast<int>(std::floor((minx - pad - L.orig[0]) / L.tile_world)), 0, L.tw - 1);
    z0 = std::clamp(static_cast<int>(std::floor((minz - pad - L.orig[2]) / L.tile_world)), 0, L.th - 1);
    x1 = std::clamp(static_cast<int>(std::floor((maxx + pad - L.orig[0]) / L.tile_world)), 0, L.tw - 1);
    z1 = std::clamp(static_cast<int>(std::floor((maxz + pad - L.orig[2]) / L.tile_world)), 0, L.th - 1);
}

std::vector<std::uint64_t> tile_hashes(const Geometry& geo, const TileLayout& L) {
    std::vector<std::uint64_t> h(static_cast<std::size_t>(L.count()), L.settings_hash);
    for (const Geometry::Source& src : geo.sources) {
        int x0, z0, x1, z1;
        tile_range(L, src.bmin[0], src.bmin[2], src.bmax[0], src.bmax[2], x0, z0, x1, z1);
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
                std::uint64_t& t = h[static_cast<std::size_t>(z) * static_cast<std::size_t>(L.tw) + static_cast<std::size_t>(x)];
                t = (t ^ src.hash) * 1099511628211ull + 0x9e3779b97f4a7c15ull;
            }
    }
    return h;
}

} // namespace

// ---- system ---------------------------------------------------------------------------------------

struct NavigationSystem::BuildJob {
    std::shared_ptr<Geometry> geo{std::make_shared<Geometry>()};
    NavMeshSurfaceComponent surface;
    std::chrono::steady_clock::time_point t0;
    TileLayout layout;                  // tiled builds
    std::vector<std::uint64_t> hashes;  // per tile
    std::vector<int> build;             // tile indices to cook (all, or the changed ones)
    bool incremental{false};
    unsigned threads{1};
};

struct NavigationSystem::Cooked {
    unsigned char* data{nullptr}; // Detour tile data (nullptr: nothing walkable)
    int size{0};
    std::uint32_t polygons{0}, vertices{0}, source_triangles{0};
    std::string error;
    NavMeshSurfaceComponent surface;
    std::chrono::steady_clock::time_point t0;
    struct Tile {
        int x{0}, z{0};
        unsigned char* data{nullptr}; // nullptr: the tile has nothing walkable (removes the old one)
        int size{0};
    };
    TileLayout layout;
    std::vector<std::uint64_t> hashes;
    std::vector<Tile> tiles;
    bool incremental{false};
    Cooked() = default;
    Cooked(const Cooked&) = delete;
    Cooked& operator=(const Cooked&) = delete;
    ~Cooked() {
        if (data) dtFree(data);
        for (Tile& t : tiles)
            if (t.data) dtFree(t.data);
    }
};

struct NavigationSystem::Impl {
    struct Agent {
        int idx{-1};
        bool character{false};
        bool has_destination{false};
        bool reached{false};
        math::Vec3 destination;
        math::Vec3 written; // last position written to the entity (detects teleports)
        NavAgentComponent params;
    };

    NavigationSettings settings;
    dtNavMesh* mesh{nullptr};
    dtNavMeshQuery* query{nullptr};
    dtCrowd* crowd{nullptr};
    dtQueryFilter filter;
    NavMeshSurfaceComponent surface;
    std::uint64_t signature{0};
    TileLayout layout;                  // of the live mesh when tiled
    std::vector<std::uint64_t> hashes;  // per tile of the live mesh
    float check_timer{0};
    std::unordered_map<Entity, Agent, ecs::EntityHash> agents;
    std::vector<Entity> scratch;
    std::future<std::shared_ptr<NavigationSystem::Cooked>> pending; // background build in flight

    void discard_pending() {
        if (pending.valid()) pending.wait();
        pending = {};
    }

    explicit Impl(NavigationSettings s) : settings(s) {
        filter.setIncludeFlags(kWalkFlag);
        filter.setExcludeFlags(0);
    }
    ~Impl() {
        discard_pending();
        release();
    }

    void release() {
        dtFreeCrowd(crowd);
        dtFreeNavMeshQuery(query);
        dtFreeNavMesh(mesh);
        crowd = nullptr, query = nullptr, mesh = nullptr;
        for (auto& [e, a] : agents) a.idx = -1;
    }

    [[nodiscard]] dtPolyRef nearest_poly(math::Vec3 p, math::Vec3 extent, float* out) const {
        if (!query) return 0;
        const float half[3]{extent.x, extent.y, extent.z};
        dtPolyRef ref = 0;
        if (dtStatusFailed(query->findNearestPoly(&p.x, half, &filter, &ref, out))) return 0;
        return ref;
    }

    [[nodiscard]] dtCrowdAgentParams agent_params(const NavAgentComponent& a) const {
        dtCrowdAgentParams p{};
        p.radius = std::max(a.radius, 0.01f);
        p.height = std::max(a.height, 0.05f);
        p.maxAcceleration = std::max(a.acceleration, 0.0f);
        p.maxSpeed = std::max(a.speed, 0.0f);
        p.collisionQueryRange = p.radius * 12.0f;
        p.pathOptimizationRange = p.radius * 30.0f;
        p.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO;
        if (a.avoidance) p.updateFlags |= DT_CROWD_OBSTACLE_AVOIDANCE | DT_CROWD_SEPARATION;
        p.obstacleAvoidanceType = 3;
        p.separationWeight = a.avoidance ? 2.0f : 0.0f;
        return p;
    }

    // Position on the navmesh corresponding to the entity's origin.
    static math::Vec3 nav_position(Scene& scene, Entity e, const Agent& a) {
        math::Vec3 p = scene.world_matrix(e).transform_point({});
        if (!a.character) p.y -= a.params.base_offset;
        return p;
    }

    bool add_to_crowd(Scene& scene, Entity e, Agent& a) {
        if (!crowd) return false;
        const math::Vec3 p = nav_position(scene, e, a);
        float snapped[3];
        if (!nearest_poly(p, {2, 4, 2}, snapped)) return false;
        const dtCrowdAgentParams params = agent_params(a.params);
        a.idx = crowd->addAgent(snapped, &params);
        if (a.idx < 0) return false;
        a.written = scene.world_matrix(e).transform_point({});
        if (a.has_destination && !a.reached) request(a);
        return true;
    }

    void remove_from_crowd(Agent& a) {
        if (crowd && a.idx >= 0) crowd->removeAgent(a.idx);
        a.idx = -1;
    }

    bool request(Agent& a) {
        if (!crowd || a.idx < 0) return false;
        float target[3];
        const dtPolyRef ref = nearest_poly(a.destination, {2, 4, 2}, target);
        if (!ref) return false;
        return crowd->requestMoveTarget(a.idx, ref, target);
    }
};

NavigationSystem::NavigationSystem(NavigationSettings settings) : impl_(std::make_unique<Impl>(settings)) {}
NavigationSystem::~NavigationSystem() = default;

bool NavigationSystem::valid() const noexcept { return impl_->mesh != nullptr; }

Status NavigationSystem::build(Scene& scene) {
    VICTUS_PROFILE_SCOPE("navigation.build");
    Impl& im = *impl_;
    im.discard_pending(); // a synchronous build supersedes a background one
    stats_.building = false;
    BuildJob job = gather(scene);
    return install(scene, cook(std::move(job)));
}

NavigationSystem::BuildJob NavigationSystem::gather(Scene& scene) {
    Impl& im = *impl_;
    BuildJob job;
    job.t0 = std::chrono::steady_clock::now();
    const NavMeshSurfaceComponent* found = surface_of(scene);
    job.surface = found ? *found : NavMeshSurfaceComponent{};
    const NavMeshSurfaceComponent& s = job.surface;
    im.signature = geometry_signature(scene, s);
    Geometry& geo = *job.geo;
    each_static_collider(scene, s.layer_mask,
                         [&](Entity e, const physics::ColliderComponent& c, bool kinematic, const physics::CollisionMesh* mesh) {
        geo.begin_source();
        if (kinematic) add_bound(geo, upright_bound(scene.world_matrix(e), c, mesh));
        else add_collider(geo, scene.world_matrix(e), c, mesh);
        geo.end_source();
    });
    each_static_heightfield(scene, s.layer_mask, [&](Entity e, const physics::StaticHeightfieldComponent& c) {
        geo.begin_source();
        add_heightfield(geo, rigid(scene.world_matrix(e)), c);
        geo.end_source();
    });
    ++stats_.builds;
    if (wants_tiles(geo, s)) {
        const rcConfig cfg = make_grid_config(geo, s, true);
        job.layout = make_tile_layout(geo, s, cfg);
        job.hashes = tile_hashes(geo, job.layout);
        job.incremental = im.mesh && job.layout.same_grid(im.layout) && im.hashes.size() == job.hashes.size();
        for (int i = 0; i < job.layout.count(); ++i)
            if (!job.incremental || job.hashes[static_cast<std::size_t>(i)] != im.hashes[static_cast<std::size_t>(i)])
                job.build.push_back(i);
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        job.threads = im.settings.build_threads ? im.settings.build_threads : std::min(hw, 8u);
    }
    return job;
}

namespace {

// One tile of a tiled navmesh: the triangles overlapping it (with the border) through the Recast
// pipeline into Detour tile data. Thread-safe (own context and buffers).
bool build_tile(const Geometry& geo, const std::vector<int>& tris, const NavMeshSurfaceComponent& s, const TileLayout& L,
                int tx, int tz, unsigned char** data, int* size, std::string& error) {
    *data = nullptr;
    *size = 0;
    if (tris.empty()) return true;
    rcConfig cfg = make_grid_config(geo, s, true);
    cfg.cs = L.cs;
    cfg.walkableRadius = static_cast<int>(std::ceil(std::max(s.agent_radius, 0.0f) / cfg.cs));
    cfg.maxEdgeLen = static_cast<int>(12.0f / cfg.cs);
    cfg.detailSampleDist = cfg.cs * 6.0f;
    cfg.tileSize = L.tile_cells;
    cfg.borderSize = L.border;
    cfg.width = cfg.height = L.tile_cells + 2 * L.border;
    float bmin[2], bmax[2];
    tile_bounds(L, tx, tz, bmin, bmax);
    cfg.bmin[0] = bmin[0], cfg.bmin[2] = bmin[1];
    cfg.bmax[0] = bmax[0], cfg.bmax[2] = bmax[1];
    rcContext ctx(false);
    struct Owned {
        rcHeightfield* hf{nullptr};
        rcCompactHeightfield* chf{nullptr};
        rcContourSet* cset{nullptr};
        rcPolyMesh* pmesh{nullptr};
        rcPolyMeshDetail* dmesh{nullptr};
        ~Owned() {
            rcFreeHeightField(hf);
            rcFreeCompactHeightfield(chf);
            rcFreeContourSet(cset);
            rcFreePolyMesh(pmesh);
            rcFreePolyMeshDetail(dmesh);
        }
    } o;
    auto fail = [&](const char* what) {
        error = std::string("navmesh tile ") + std::to_string(tx) + "," + std::to_string(tz) + " failed: " + what;
        return false;
    };
    const int nverts = static_cast<int>(geo.verts.size() / 3), ntris = static_cast<int>(tris.size() / 3);
    o.hf = rcAllocHeightfield();
    if (!o.hf || !rcCreateHeightfield(&ctx, *o.hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) return fail("heightfield");
    std::vector<unsigned char> areas(static_cast<std::size_t>(ntris), 0);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, geo.verts.data(), nverts, tris.data(), ntris, areas.data());
    if (!rcRasterizeTriangles(&ctx, geo.verts.data(), nverts, tris.data(), areas.data(), ntris, *o.hf, cfg.walkableClimb))
        return fail("rasterize");
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *o.hf);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *o.hf);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *o.hf);
    o.chf = rcAllocCompactHeightfield();
    if (!o.chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *o.hf, *o.chf)) return fail("compact heightfield");
    rcFreeHeightField(o.hf);
    o.hf = nullptr;
    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *o.chf)) return fail("erode");
    if (!rcBuildDistanceField(&ctx, *o.chf)) return fail("distance field");
    if (!rcBuildRegions(&ctx, *o.chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea)) return fail("regions");
    o.cset = rcAllocContourSet();
    if (!o.cset || !rcBuildContours(&ctx, *o.chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *o.cset)) return fail("contours");
    if (o.cset->nconts == 0) return true;
    o.pmesh = rcAllocPolyMesh();
    if (!o.pmesh || !rcBuildPolyMesh(&ctx, *o.cset, cfg.maxVertsPerPoly, *o.pmesh)) return fail("polygon mesh");
    o.dmesh = rcAllocPolyMeshDetail();
    if (!o.dmesh || !rcBuildPolyMeshDetail(&ctx, *o.pmesh, *o.chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *o.dmesh))
        return fail("detail mesh");
    if (o.pmesh->npolys == 0) return true;
    if (o.pmesh->nverts >= 0xffff) return fail("too many vertices (lower tile_size)");
    for (int i = 0; i < o.pmesh->npolys; ++i)
        if (o.pmesh->areas[i] == RC_WALKABLE_AREA) o.pmesh->flags[i] = kWalkFlag;
    dtNavMeshCreateParams p{};
    p.verts = o.pmesh->verts;
    p.vertCount = o.pmesh->nverts;
    p.polys = o.pmesh->polys;
    p.polyAreas = o.pmesh->areas;
    p.polyFlags = o.pmesh->flags;
    p.polyCount = o.pmesh->npolys;
    p.nvp = o.pmesh->nvp;
    p.detailMeshes = o.dmesh->meshes;
    p.detailVerts = o.dmesh->verts;
    p.detailVertsCount = o.dmesh->nverts;
    p.detailTris = o.dmesh->tris;
    p.detailTriCount = o.dmesh->ntris;
    p.walkableHeight = s.agent_height;
    p.walkableRadius = s.agent_radius;
    p.walkableClimb = s.max_climb;
    p.tileX = tx;
    p.tileY = tz;
    p.tileLayer = 0;
    rcVcopy(p.bmin, o.pmesh->bmin);
    rcVcopy(p.bmax, o.pmesh->bmax);
    p.cs = cfg.cs;
    p.ch = cfg.ch;
    p.buildBvTree = true;
    if (!dtCreateNavMeshData(&p, data, size)) return fail("detour data");
    return true;
}

} // namespace

// Cooks the job's tiles on up to `threads` threads. Triangles are bucketed per tile first, so each
// tile rasterizes only what overlaps it.
std::shared_ptr<NavigationSystem::Cooked> NavigationSystem::cook_tiles(BuildJob job, std::shared_ptr<Cooked> out) {
    const Geometry& geo = *job.geo;
    const TileLayout& L = job.layout;
    out->layout = L;
    out->hashes = job.hashes;
    out->incremental = job.incremental;
    std::vector<int> slot(static_cast<std::size_t>(L.count()), -1);
    for (std::size_t i = 0; i < job.build.size(); ++i) slot[static_cast<std::size_t>(job.build[i])] = static_cast<int>(i);
    std::vector<std::vector<int>> buckets(job.build.size());
    const int ntris = geo.triangle_count();
    for (int t = 0; t < ntris; ++t) {
        const int* idx = &geo.tris[static_cast<std::size_t>(t) * 3];
        float mnx = FLT_MAX, mnz = FLT_MAX, mxx = -FLT_MAX, mxz = -FLT_MAX;
        for (int k = 0; k < 3; ++k) {
            const float* v = &geo.verts[static_cast<std::size_t>(idx[k]) * 3];
            mnx = std::min(mnx, v[0]), mxx = std::max(mxx, v[0]);
            mnz = std::min(mnz, v[2]), mxz = std::max(mxz, v[2]);
        }
        int x0, z0, x1, z1;
        tile_range(L, mnx, mnz, mxx, mxz, x0, z0, x1, z1);
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
                const int sl = slot[static_cast<std::size_t>(z) * static_cast<std::size_t>(L.tw) + static_cast<std::size_t>(x)];
                if (sl >= 0) buckets[static_cast<std::size_t>(sl)].insert(buckets[static_cast<std::size_t>(sl)].end(), idx, idx + 3);
            }
    }
    out->tiles.resize(job.build.size());
    std::vector<std::string> errors(job.build.size());
    std::atomic<std::size_t> next{0};
    auto worker = [&] {
        for (std::size_t i = next.fetch_add(1); i < job.build.size(); i = next.fetch_add(1)) {
            Cooked::Tile& t = out->tiles[i];
            t.x = job.build[i] % L.tw;
            t.z = job.build[i] / L.tw;
            (void)build_tile(geo, buckets[i], job.surface, L, t.x, t.z, &t.data, &t.size, errors[i]);
        }
    };
    const unsigned n = std::clamp<unsigned>(job.threads, 1u, static_cast<unsigned>(std::max<std::size_t>(job.build.size(), 1)));
    std::vector<std::thread> pool;
    for (unsigned i = 1; i < n; ++i) pool.emplace_back(worker);
    worker();
    for (std::thread& th : pool) th.join();
    for (const std::string& e : errors)
        if (!e.empty()) {
            out->error = e;
            break;
        }
    return out;
}

// Recast/Detour pipeline on gathered geometry. Touches no scene or system state, so it may run on a
// worker thread.
std::shared_ptr<NavigationSystem::Cooked> NavigationSystem::cook(BuildJob job) {
    auto out = std::make_shared<Cooked>();
    out->surface = job.surface;
    out->t0 = job.t0;
    const Geometry& geo = *job.geo;
    const NavMeshSurfaceComponent& s = job.surface;
    out->source_triangles = static_cast<std::uint32_t>(geo.triangle_count());
    if (geo.tris.empty()) return out; // nothing to walk on: queries fail, agents stay put
    if (job.layout.tiled) return cook_tiles(std::move(job), std::move(out));

    rcConfig cfg = make_grid_config(geo, s);
    rcContext ctx(false);
    struct Owned {
        rcHeightfield* hf{nullptr};
        rcCompactHeightfield* chf{nullptr};
        rcContourSet* cset{nullptr};
        rcPolyMesh* pmesh{nullptr};
        rcPolyMeshDetail* dmesh{nullptr};
        ~Owned() {
            rcFreeHeightField(hf);
            rcFreeCompactHeightfield(chf);
            rcFreeContourSet(cset);
            rcFreePolyMesh(pmesh);
            rcFreePolyMeshDetail(dmesh);
        }
    } o;
    auto fail = [&](const char* what) {
        out->error = std::string("navmesh build failed: ") + what;
        return out;
    };
    const int nverts = static_cast<int>(geo.verts.size() / 3), ntris = geo.triangle_count();
    o.hf = rcAllocHeightfield();
    if (!o.hf || !rcCreateHeightfield(&ctx, *o.hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
        return fail("heightfield");
    std::vector<unsigned char> areas(static_cast<std::size_t>(ntris), 0);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, geo.verts.data(), nverts, geo.tris.data(), ntris, areas.data());
    if (!rcRasterizeTriangles(&ctx, geo.verts.data(), nverts, geo.tris.data(), areas.data(), ntris, *o.hf, cfg.walkableClimb))
        return fail("rasterize");
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *o.hf);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *o.hf);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *o.hf);
    o.chf = rcAllocCompactHeightfield();
    if (!o.chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *o.hf, *o.chf))
        return fail("compact heightfield");
    rcFreeHeightField(o.hf);
    o.hf = nullptr; // the span heightfield is the largest allocation; free it early
    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *o.chf)) return fail("erode");
    if (!rcBuildDistanceField(&ctx, *o.chf)) return fail("distance field");
    if (!rcBuildRegions(&ctx, *o.chf, 0, cfg.minRegionArea, cfg.mergeRegionArea)) return fail("regions");
    o.cset = rcAllocContourSet();
    if (!o.cset || !rcBuildContours(&ctx, *o.chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *o.cset)) return fail("contours");
    o.pmesh = rcAllocPolyMesh();
    if (!o.pmesh || !rcBuildPolyMesh(&ctx, *o.cset, cfg.maxVertsPerPoly, *o.pmesh)) return fail("polygon mesh");
    o.dmesh = rcAllocPolyMeshDetail();
    if (!o.dmesh || !rcBuildPolyMeshDetail(&ctx, *o.pmesh, *o.chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *o.dmesh))
        return fail("detail mesh");
    if (o.pmesh->npolys == 0) return out; // geometry exists but nothing is walkable for this agent size
    if (o.pmesh->nverts >= 0xffff) return fail("too many vertices for a single tile (increase cell_size)");
    for (int i = 0; i < o.pmesh->npolys; ++i) {
        if (o.pmesh->areas[i] == RC_WALKABLE_AREA) o.pmesh->flags[i] = kWalkFlag;
    }

    dtNavMeshCreateParams p{};
    p.verts = o.pmesh->verts;
    p.vertCount = o.pmesh->nverts;
    p.polys = o.pmesh->polys;
    p.polyAreas = o.pmesh->areas;
    p.polyFlags = o.pmesh->flags;
    p.polyCount = o.pmesh->npolys;
    p.nvp = o.pmesh->nvp;
    p.detailMeshes = o.dmesh->meshes;
    p.detailVerts = o.dmesh->verts;
    p.detailVertsCount = o.dmesh->nverts;
    p.detailTris = o.dmesh->tris;
    p.detailTriCount = o.dmesh->ntris;
    p.walkableHeight = s.agent_height;
    p.walkableRadius = s.agent_radius;
    p.walkableClimb = s.max_climb;
    rcVcopy(p.bmin, o.pmesh->bmin);
    rcVcopy(p.bmax, o.pmesh->bmax);
    p.cs = cfg.cs;
    p.ch = cfg.ch;
    p.buildBvTree = true;
    if (!dtCreateNavMeshData(&p, &out->data, &out->size)) return fail("detour data");
    out->polygons = static_cast<std::uint32_t>(o.pmesh->npolys);
    out->vertices = static_cast<std::uint32_t>(o.pmesh->nverts);
    return out;
}

// Replaces the live navmesh with a cooked one (main thread).
Status NavigationSystem::install(Scene& scene, std::shared_ptr<Cooked> c) {
    Impl& im = *impl_;
    im.surface = c->surface;
    stats_.source_triangles = c->source_triangles;
    if (c->layout.tiled && c->error.empty()) return install_tiles(scene, std::move(c));
    im.release();
    stats_.valid = false;
    stats_.polygons = stats_.vertices = 0;
    auto finish = [&] {
        stats_.build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c->t0).count();
    };
    auto fail = [&](std::string what) -> Status {
        finish();
        return Error{ErrorCode::Unknown, std::move(what)};
    };
    if (!c->error.empty()) return fail(c->error);
    im.layout = {};
    im.hashes.clear();
    stats_.tiles = c->data ? 1 : 0;
    stats_.tiles_rebuilt = stats_.tiles;
    if (!c->data) {
        finish();
        return {};
    }
    im.mesh = dtAllocNavMesh();
    if (!im.mesh || dtStatusFailed(im.mesh->init(c->data, c->size, DT_TILE_FREE_DATA))) {
        im.release();
        return fail("navmesh build failed: detour init");
    }
    c->data = nullptr; // owned by the navmesh now
    return finish_install(scene, *c);
}

// Query and crowd on the live mesh, agents re-registered (shared by single-tile and tiled installs).
Status NavigationSystem::finish_install(Scene& scene, const Cooked& c) {
    Impl& im = *impl_;
    const NavMeshSurfaceComponent& s = im.surface;
    auto finish = [&] {
        stats_.build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c.t0).count();
    };
    auto fail = [&](std::string what) -> Status {
        finish();
        return Error{ErrorCode::Unknown, std::move(what)};
    };
    im.query = dtAllocNavMeshQuery();
    if (!im.query || dtStatusFailed(im.query->init(im.mesh, im.layout.tiled ? 16384 : 4096))) {
        im.release();
        return fail("navmesh build failed: query init");
    }
    im.crowd = dtAllocCrowd();
    float max_radius = s.agent_radius;
    scene.world().each<NavAgentComponent>([&](Entity, NavAgentComponent& a) { max_radius = std::max(max_radius, a.radius); });
    if (!im.crowd || !im.crowd->init(static_cast<int>(im.settings.max_agents), std::max(max_radius, 0.05f), im.mesh)) {
        im.release();
        return fail("navmesh build failed: crowd init");
    }
    // Avoidance quality presets (DetourCrowd sample values): low, medium, good, high.
    dtObstacleAvoidanceParams oa = *im.crowd->getObstacleAvoidanceParams(0);
    oa.velBias = 0.5f;
    oa.adaptiveDivs = 5, oa.adaptiveRings = 2, oa.adaptiveDepth = 1;
    im.crowd->setObstacleAvoidanceParams(0, &oa);
    oa.adaptiveDivs = 5, oa.adaptiveRings = 2, oa.adaptiveDepth = 2;
    im.crowd->setObstacleAvoidanceParams(1, &oa);
    oa.adaptiveDivs = 7, oa.adaptiveRings = 2, oa.adaptiveDepth = 3;
    im.crowd->setObstacleAvoidanceParams(2, &oa);
    oa.adaptiveDivs = 7, oa.adaptiveRings = 3, oa.adaptiveDepth = 3;
    im.crowd->setObstacleAvoidanceParams(3, &oa);

    stats_.valid = true;
    stats_.polygons = c.polygons;
    stats_.vertices = c.vertices;
    // Re-register agents on the new mesh, keeping their destinations.
    for (auto& [e, a] : im.agents) {
        if (scene.world().alive(e)) (void)im.add_to_crowd(scene, e, a);
    }
    finish();
    return {};
}

// Installs a tiled build: swaps only the cooked tiles into the live mesh when the grid is
// unchanged (incremental), otherwise creates a new multi-tile mesh.
Status NavigationSystem::install_tiles(Scene& scene, std::shared_ptr<Cooked> c) {
    Impl& im = *impl_;
    const TileLayout& L = c->layout;
    auto count_mesh = [&] {
        c->polygons = c->vertices = 0;
        std::uint32_t tiles = 0;
        const dtNavMesh& m = *im.mesh;
        for (int t = 0; t < m.getMaxTiles(); ++t) {
            const dtMeshTile* tile = m.getTile(t);
            if (!tile || !tile->header) continue;
            ++tiles;
            c->polygons += static_cast<std::uint32_t>(tile->header->polyCount);
            c->vertices += static_cast<std::uint32_t>(tile->header->vertCount);
        }
        stats_.tiles = tiles;
    };
    auto add_tiles = [&]() -> bool {
        for (Cooked::Tile& t : c->tiles) {
            if (const dtTileRef ref = im.mesh->getTileRefAt(t.x, t.z, 0)) im.mesh->removeTile(ref, nullptr, nullptr);
            if (!t.data) continue;
            if (dtStatusFailed(im.mesh->addTile(t.data, t.size, DT_TILE_FREE_DATA, 0, nullptr))) return false;
            t.data = nullptr; // owned by the navmesh
        }
        return true;
    };
    stats_.tiles_rebuilt = static_cast<std::uint32_t>(c->tiles.size());
    if (c->incremental && im.mesh && im.crowd && L.same_grid(im.layout)) {
        // Agents on swapped tiles hold stale polygon refs: re-register everyone on the patched mesh.
        for (auto& [e, a] : im.agents) im.remove_from_crowd(a);
        if (!add_tiles()) {
            im.signature = 0; // force a full rebuild next time
            return Error{ErrorCode::Unknown, "navmesh build failed: detour add tile"};
        }
        im.hashes = c->hashes;
        count_mesh();
        stats_.valid = true;
        stats_.polygons = c->polygons;
        stats_.vertices = c->vertices;
        for (auto& [e, a] : im.agents)
            if (scene.world().alive(e)) (void)im.add_to_crowd(scene, e, a);
        stats_.build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c->t0).count();
        return {};
    }
    if (c->incremental) im.signature = 0; // the grid changed while cooking: rebuild everything next check
    im.release();
    stats_.valid = false;
    stats_.polygons = stats_.vertices = 0;
    im.layout = L;
    im.hashes = c->hashes;
    im.mesh = dtAllocNavMesh();
    dtNavMeshParams params{};
    rcVcopy(params.orig, L.orig);
    params.tileWidth = L.tile_world;
    params.tileHeight = L.tile_world;
    params.maxTiles = L.count();
    params.maxPolys = L.max_polys;
    if (!im.mesh || dtStatusFailed(im.mesh->init(&params)) || !add_tiles()) {
        im.release();
        return Error{ErrorCode::Unknown, "navmesh build failed: detour tiled init"};
    }
    count_mesh();
    if (stats_.tiles == 0) { // nothing walkable anywhere
        im.release();
        stats_.build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c->t0).count();
        return {};
    }
    return finish_install(scene, *c);
}

Status NavigationSystem::ensure_built(Scene& scene) {
    Impl& im = *impl_;
    if (im.pending.valid()) { // a background build is in flight: finish it now
        auto c = im.pending.get();
        stats_.building = false;
        if (auto st = install(scene, std::move(c)); !st) return st;
    }
    const NavMeshSurfaceComponent* found = surface_of(scene);
    const NavMeshSurfaceComponent s = found ? *found : NavMeshSurfaceComponent{};
    if (stats_.builds > 0 && geometry_signature(scene, s) == im.signature) return {};
    return build(scene);
}

Status NavigationSystem::ensure_built_async(Scene& scene) {
    Impl& im = *impl_;
    if (im.pending.valid()) {
        if (im.pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return {};
        auto c = im.pending.get();
        stats_.building = false;
        const Status st = install(scene, std::move(c));
        if (st) VICTUS_LOG_INFO("navigation", "navmesh built in the background: {} polygons in {:.0f} ms", stats_.polygons, stats_.build_ms);
        return st;
    }
    const NavMeshSurfaceComponent* found = surface_of(scene);
    const NavMeshSurfaceComponent s = found ? *found : NavMeshSurfaceComponent{};
    if (stats_.builds > 0 && geometry_signature(scene, s) == im.signature) return {};
    VICTUS_PROFILE_SCOPE("navigation.build");
    BuildJob job = gather(scene);
    const rcConfig cfg = make_grid_config(*job.geo, job.surface);
    if (job.geo->tris.empty() || static_cast<double>(cfg.width) * cfg.height < im.settings.async_min_cells)
        return install(scene, cook(std::move(job)));
    // Large levels (a square kilometre of terrain takes seconds) cook on a worker thread; the
    // previous navmesh, if any, keeps answering queries until the new one is installed.
    stats_.building = true;
    im.pending = std::async(std::launch::async, [job = std::move(job)]() mutable { return cook(std::move(job)); });
    return {};
}

bool NavigationSystem::building() const noexcept { return impl_->pending.valid(); }

bool NavigationSystem::find_path(math::Vec3 from, math::Vec3 to, std::vector<math::Vec3>& out, bool* partial) const {
    out.clear();
    if (partial) *partial = false;
    const Impl& im = *impl_;
    float spos[3], epos[3];
    const dtPolyRef sref = im.nearest_poly(from, {2, 4, 2}, spos);
    const dtPolyRef eref = im.nearest_poly(to, {2, 4, 2}, epos);
    if (!sref || !eref) return false;
    dtPolyRef polys[kMaxPathPolys];
    int npolys = 0;
    if (dtStatusFailed(im.query->findPath(sref, eref, spos, epos, &im.filter, polys, &npolys, kMaxPathPolys)) || npolys == 0)
        return false;
    float end[3];
    dtVcopy(end, epos);
    if (polys[npolys - 1] != eref) {
        if (partial) *partial = true;
        im.query->closestPointOnPoly(polys[npolys - 1], epos, end, nullptr);
    }
    float straight[kMaxPathPolys * 3];
    int nstraight = 0;
    if (dtStatusFailed(im.query->findStraightPath(spos, end, polys, npolys, straight, nullptr, nullptr, &nstraight,
                                                  kMaxPathPolys, 0)))
        return false;
    for (int i = 0; i < nstraight; ++i) out.push_back(vec(straight + 3 * i));
    return !out.empty();
}

std::optional<math::Vec3> NavigationSystem::nearest(math::Vec3 point, math::Vec3 extent) const {
    float p[3];
    if (!impl_->nearest_poly(point, extent, p)) return std::nullopt;
    return vec(p);
}

RaycastResult NavigationSystem::raycast(math::Vec3 from, math::Vec3 to) const {
    RaycastResult r;
    const Impl& im = *impl_;
    float spos[3];
    const dtPolyRef sref = im.nearest_poly(from, {2, 4, 2}, spos);
    if (!sref) {
        r.hit = true;
        r.point = from;
        return r;
    }
    float t = 0, normal[3]{0, 0, 0};
    dtPolyRef path[kMaxPathPolys];
    int npath = 0;
    if (dtStatusFailed(im.query->raycast(sref, spos, &to.x, &im.filter, &t, normal, path, &npath, kMaxPathPolys))) {
        r.hit = true;
        r.point = vec(spos);
        return r;
    }
    const math::Vec3 start = vec(spos);
    if (t >= 1.0f) {
        r.point = to;
        // Project the end onto the surface height.
        float h = to.y;
        if (npath > 0 && dtStatusSucceed(im.query->getPolyHeight(path[npath - 1], &to.x, &h))) r.point.y = h;
        return r;
    }
    r.hit = true;
    r.point = start + (to - start) * t;
    r.normal = vec(normal);
    return r;
}

std::optional<math::Vec3> NavigationSystem::random_point(std::uint32_t seed) const {
    const Impl& im = *impl_;
    if (!im.query) return std::nullopt;
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    std::mt19937 rng(seed);
    g_random = &rng;
    dtPolyRef ref = 0;
    float p[3];
    const dtStatus st = im.query->findRandomPoint(&im.filter, random_unit, &ref, p);
    g_random = nullptr;
    if (dtStatusFailed(st) || !ref) return std::nullopt;
    return vec(p);
}

bool NavigationSystem::set_destination(Scene& scene, Entity agent, math::Vec3 destination) {
    if (!scene.world().alive(agent) || !scene.world().has<NavAgentComponent>(agent)) return false;
    Impl& im = *impl_;
    if (!im.mesh) (void)ensure_built(scene);
    auto& a = im.agents[agent];
    a.params = *scene.world().get<NavAgentComponent>(agent);
    a.character = scene.world().has<physics::CharacterControllerComponent>(agent);
    a.destination = destination;
    a.has_destination = true;
    a.reached = false;
    if (a.idx < 0) return im.add_to_crowd(scene, agent, a) && a.idx >= 0;
    return im.request(a);
}

void NavigationSystem::stop(Scene& scene, Entity agent) {
    Impl& im = *impl_;
    const auto it = im.agents.find(agent);
    if (it == im.agents.end()) return;
    it->second.has_destination = false;
    it->second.reached = false;
    if (im.crowd && it->second.idx >= 0) im.crowd->resetMoveTarget(it->second.idx);
    if (it->second.character) {
        if (auto* w = physics::world_of(scene)) w->move_character(agent, {});
    }
}

AgentState NavigationSystem::agent_state(Entity agent) const {
    AgentState s;
    const Impl& im = *impl_;
    const auto it = im.agents.find(agent);
    if (it == im.agents.end()) return s;
    const auto& a = it->second;
    s.active = a.idx >= 0 && im.crowd;
    s.has_destination = a.has_destination;
    s.reached = a.reached;
    s.destination = a.destination;
    if (s.active) {
        const dtCrowdAgent* ca = im.crowd->getAgent(a.idx);
        s.velocity = vec(ca->vel);
        s.partial = ca->partial;
        s.remaining_distance = a.has_destination && !a.reached ? math::length(a.destination - vec(ca->npos)) : 0.0f;
    }
    return s;
}

void NavigationSystem::update(Scene& scene, float dt) {
    VICTUS_PROFILE_SCOPE("navigation.update");
    Impl& im = *impl_;
    ecs::World& w = scene.world();
    stats_.agents = stats_.moving = 0;

    // Agents present in the scene.
    bool any_agents = false;
    w.each<NavAgentComponent>([&](Entity, NavAgentComponent&) { any_agents = true; });
    if (!any_agents && im.agents.empty()) return;

    // Build on first use, then rebuild periodically when static geometry changes.
    im.check_timer -= dt;
    if (stats_.builds == 0 || im.pending.valid() || (im.surface.auto_rebuild && im.check_timer <= 0)) {
        im.check_timer = im.settings.rebuild_check_interval;
        if (auto st = ensure_built_async(scene); !st) VICTUS_LOG_WARN("navigation", "{}", st.error().message);
    }
    if (!im.crowd) return;

    // Drop agents whose entity or component disappeared.
    im.scratch.clear();
    for (auto& [e, a] : im.agents) {
        if (!w.alive(e) || !w.has<NavAgentComponent>(e)) im.scratch.push_back(e);
    }
    for (Entity e : im.scratch) {
        im.remove_from_crowd(im.agents[e]);
        im.agents.erase(e);
    }

    physics::PhysicsWorld* physics_world = physics::world_of(scene);
    // Register new agents, apply parameter edits and follow external moves (teleports, characters).
    w.each<NavAgentComponent>([&](Entity e, NavAgentComponent& c) {
        auto& a = im.agents[e];
        const bool character = w.has<physics::CharacterControllerComponent>(e);
        if (a.idx < 0) {
            a.params = c;
            a.character = character;
            (void)im.add_to_crowd(scene, e, a);
            return;
        }
        if (!(a.params == c) || a.character != character) {
            a.params = c;
            a.character = character;
            const dtCrowdAgentParams p = im.agent_params(c);
            im.crowd->updateAgentParameters(a.idx, &p);
        }
        const math::Vec3 origin = scene.world_matrix(e).transform_point({});
        if (math::length(origin - a.written) > 1e-3f) {
            dtCrowdAgent* ca = im.crowd->getEditableAgent(a.idx);
            const math::Vec3 target = Impl::nav_position(scene, e, a);
            const float drift = math::length(math::Vec3{target.x - ca->npos[0], 0, target.z - ca->npos[2]});
            if (a.character && drift < 1.0f && ca->corridor.getPathCount() > 0) {
                ca->corridor.movePosition(&target.x, im.query, &im.filter);
                dtVcopy(ca->npos, ca->corridor.getPos());
            } else {
                // Teleported (or far off the mesh): re-place it.
                im.remove_from_crowd(a);
                (void)im.add_to_crowd(scene, e, a);
            }
            a.written = origin;
        }
    });

    im.crowd->update(dt, nullptr);

    // Arrival, then write positions/velocities back.
    for (auto& [e, a] : im.agents) {
        if (a.idx < 0) continue;
        ++stats_.agents;
        dtCrowdAgent* ca = im.crowd->getEditableAgent(a.idx);
        if (a.has_destination && !a.reached) {
            const math::Vec3 pos = vec(ca->npos);
            float goal[3];
            const math::Vec3 target = im.nearest_poly(a.destination, {2, 4, 2}, goal) ? vec(goal) : a.destination;
            const float dxz = math::length(math::Vec3{target.x - pos.x, 0, target.z - pos.z});
            const bool failed = ca->targetState == DT_CROWDAGENT_TARGET_FAILED;
            // Partial paths end at the closest reachable point: arrive at the corridor's end instead.
            const math::Vec3 end = vec(ca->corridor.getTarget());
            const bool at_partial_end = ca->partial && ca->targetState == DT_CROWDAGENT_TARGET_VALID &&
                                        math::length(math::Vec3{end.x - pos.x, 0, end.z - pos.z}) <= a.params.stopping_distance;
            if (dxz <= std::max(a.params.stopping_distance, 0.01f) || failed || at_partial_end) {
                a.reached = true;
                im.crowd->resetMoveTarget(a.idx);
                dtVset(ca->vel, 0, 0, 0);
                dtVset(ca->dvel, 0, 0, 0);
            }
        }
        const math::Vec3 vel = vec(ca->vel);
        const float speed_xz = std::sqrt(vel.x * vel.x + vel.z * vel.z);
        if (speed_xz > 0.01f) ++stats_.moving;

        auto* t = w.get<scene::Transform>(e);
        if (!t) continue;
        if (a.character) {
            if (physics_world) physics_world->move_character(e, a.reached || !a.has_destination ? math::Vec3{} : math::Vec3{vel.x, 0, vel.z});
        } else {
            math::Vec3 p = vec(ca->npos);
            p.y += a.params.base_offset;
            const Entity parent = scene.parent(e);
            t->position = parent.is_null() ? p : math::inverse(scene.world_matrix(parent)).transform_point(p);
            a.written = p;
        }
        if (a.params.rotate && speed_xz > 0.05f && scene.parent(e).is_null()) {
            const float want = std::atan2(vel.x, vel.z);
            const float have = math::to_euler(t->rotation).y;
            const float max_step = a.params.turn_speed * (kPi / 180.0f) * dt;
            const float step = std::clamp(shortest_angle(have, want), -max_step, max_step);
            t->rotation = math::Quat::euler(0, have + step, 0);
        }
        if (a.character) a.written = scene.world_matrix(e).transform_point({});
    }
}

void NavigationSystem::debug_lines(std::vector<DebugLine>& out, bool include_paths) const {
    const Impl& im = *impl_;
    if (!im.mesh) return;
    constexpr math::Color kEdge{0.2f, 0.75f, 1.0f, 1.0f};
    constexpr math::Color kInner{0.2f, 0.75f, 1.0f, 0.35f};
    constexpr math::Color kPath{1.0f, 0.85f, 0.2f, 1.0f};
    constexpr float kLift = 0.03f; // draw slightly above the surface to avoid z-fighting
    const dtNavMesh& mesh = *im.mesh;
    for (int t = 0; t < mesh.getMaxTiles(); ++t) {
        const dtMeshTile* tile = mesh.getTile(t);
        if (!tile || !tile->header) continue;
        for (int i = 0; i < tile->header->polyCount; ++i) {
            const dtPoly& poly = tile->polys[i];
            if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) continue;
            for (int j = 0; j < poly.vertCount; ++j) {
                const int k = (j + 1) % poly.vertCount;
                // Each internal edge is shared by two polygons: draw it once.
                const int nei = poly.neis[j];
                const bool boundary = nei == 0 || (nei & DT_EXT_LINK);
                if (!boundary && nei - 1 < i) continue;
                math::Vec3 a = vec(&tile->verts[poly.verts[j] * 3]), b = vec(&tile->verts[poly.verts[k] * 3]);
                a.y += kLift, b.y += kLift;
                out.push_back({a, b, boundary ? kEdge : kInner});
            }
        }
    }
    if (!include_paths || !im.crowd) return;
    for (const auto& [e, a] : im.agents) {
        if (a.idx < 0 || !a.has_destination || a.reached) continue;
        const dtCrowdAgent* ca = im.crowd->getAgent(a.idx);
        math::Vec3 prev = vec(ca->npos);
        for (int i = 0; i < ca->ncorners; ++i) {
            const math::Vec3 c = vec(&ca->cornerVerts[i * 3]);
            out.push_back({prev + math::Vec3{0, kLift * 2, 0}, c + math::Vec3{0, kLift * 2, 0}, kPath});
            prev = c;
        }
    }
}

// ---- registration and binding ---------------------------------------------------------------------

void register_components() {
    static std::once_flag once;
    std::call_once(once, [] {
        scene::Schema<NavMeshSurfaceComponent>("NavMeshSurface", "Navigation")
            .field(&NavMeshSurfaceComponent::agent_radius, "agent_radius", {.min = 0, .max = 10, .speed = 0.01f, .tooltip = "Clearance kept from walls (m)"})
            .field(&NavMeshSurfaceComponent::agent_height, "agent_height", {.min = 0.1f, .max = 20, .speed = 0.01f, .tooltip = "Minimum free height (m)"})
            .field(&NavMeshSurfaceComponent::max_climb, "max_climb", {.min = 0, .max = 5, .speed = 0.01f, .tooltip = "Highest step walked up or down (m)"})
            .field(&NavMeshSurfaceComponent::max_slope, "max_slope", {.min = 0, .max = 89, .speed = 0.5f, .hint = "degrees"})
            .field(&NavMeshSurfaceComponent::cell_size, "cell_size", {.min = 0.02f, .max = 2, .speed = 0.005f, .tooltip = "Voxel size: smaller is more accurate and slower to build"})
            .field(&NavMeshSurfaceComponent::cell_height, "cell_height", {.min = 0.02f, .max = 2, .speed = 0.005f})
            .field(&NavMeshSurfaceComponent::layer_mask, "layer_mask", {.tooltip = "Collider layers that contribute walkable geometry", .hint = "bitmask16"})
            .field(&NavMeshSurfaceComponent::auto_rebuild, "auto_rebuild", {.tooltip = "Rebuild when static colliders move or change"})
            .field(&NavMeshSurfaceComponent::tile_size, "tile_size", {.min = 0, .max = 1000, .speed = 0.5f, .label = "Tile Size",
                   .tooltip = "Metres per navmesh tile; 0 = automatic (one tile for small levels, tiles of 256 voxels above a million voxel columns). Tiled meshes rebuild only the tiles a change touches"})
            .commit();
        scene::Schema<NavAgentComponent>("NavAgent", "Navigation")
            .field(&NavAgentComponent::speed, "speed", {.min = 0, .max = 100, .speed = 0.05f, .tooltip = "m/s"})
            .field(&NavAgentComponent::acceleration, "acceleration", {.min = 0, .max = 1000, .speed = 0.1f, .tooltip = "m/s^2"})
            .field(&NavAgentComponent::radius, "radius", {.min = 0.01f, .max = 10, .speed = 0.01f})
            .field(&NavAgentComponent::height, "height", {.min = 0.05f, .max = 20, .speed = 0.01f})
            .field(&NavAgentComponent::stopping_distance, "stopping_distance", {.min = 0, .max = 100, .speed = 0.01f})
            .field(&NavAgentComponent::base_offset, "base_offset", {.min = -100, .max = 100, .speed = 0.01f, .tooltip = "Entity origin height above the navmesh (agents without a CharacterController)"})
            .field(&NavAgentComponent::avoidance, "avoidance", {.tooltip = "Steer around other agents"})
            .field(&NavAgentComponent::rotate, "rotate", {.tooltip = "Turn to face the movement direction"})
            .field(&NavAgentComponent::turn_speed, "turn_speed", {.min = 0, .max = 5000, .speed = 1, .tooltip = "degrees per second"})
            .commit();
    });
}

namespace {
struct Binding {
    NavigationSystem* nav{nullptr};
};
} // namespace

void bind(Scene& scene, NavigationSystem* nav) {
    if (auto* b = scene.world().resource<Binding>()) b->nav = nav;
    else scene.world().set_resource<Binding>().nav = nav;
}

NavigationSystem* navigation_of(Scene& scene) {
    const auto* b = scene.world().resource<Binding>();
    return b ? b->nav : nullptr;
}

void add_navigation_systems(gameplay::Simulation& sim, NavigationSystem& nav) {
    register_components();
    NavigationSystem* n = &nav;
    sim.add_system("navigation.bind", gameplay::Phase::PreUpdate, [n](Scene& scene, const gameplay::FrameTime&) {
        if (navigation_of(scene) != n) bind(scene, n);
    });
    sim.add_system("navigation.update", gameplay::Phase::Update, [n](Scene& scene, const gameplay::FrameTime& t) {
        if (navigation_of(scene) != n) bind(scene, n);
        n->update(scene, t.dt);
    });
}

} // namespace victus::navigation
