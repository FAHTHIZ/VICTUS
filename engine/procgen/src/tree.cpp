// SPDX-License-Identifier: Apache-2.0
// Procedural trees (ADR-0243).
#include "victus/procgen/tree.hpp"

#include "victus/core/hash.hpp"
#include "victus/core/log.hpp"
#include "victus/core/profiler.hpp"
#include "victus/scene/reflect.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <numbers>
#include <utility>
#include <vector>

namespace victus::procgen {

using math::Vec3;
using scene::Entity;

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

std::uint32_t hash32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}
// Uniform in [0, 1) for stream `i` of `seed`.
float rnd(std::uint32_t seed, std::uint32_t i) { return static_cast<float>(hash32(seed * 0x9E3779B1u + hash32(i + 0x632BE5ABu)) >> 8) / 16777216.0f; }

// sRGB colour scaled by `f` (and a warm/cool shift `w`), packed RGBA8 for ProceduralMesh::colors.
std::uint32_t pack(math::Color c, float f, float w = 0.0f) {
    const float k[3] = {c.r * f * (1.0f + 0.06f * w), c.g * f, c.b * f * (1.0f - 0.06f * w)};
    std::uint32_t out = 0xffu << 24;
    for (int i = 0; i < 3; ++i)
        out |= static_cast<std::uint32_t>(std::clamp(k[i] * 255.0f + 0.5f, 0.0f, 255.0f)) << (i * 8);
    return out;
}

// Unit icosphere subdivided `levels` times (level 2: 162 vertices, 320 triangles).
struct Icosphere {
    std::vector<Vec3> verts;
    std::vector<std::uint32_t> tris;
};
const Icosphere& icosphere() {
    static const Icosphere sphere = [] {
        Icosphere s;
        const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
        for (const Vec3 v : {Vec3{-1, t, 0}, Vec3{1, t, 0}, Vec3{-1, -t, 0}, Vec3{1, -t, 0}, Vec3{0, -1, t}, Vec3{0, 1, t},
                             Vec3{0, -1, -t}, Vec3{0, 1, -t}, Vec3{t, 0, -1}, Vec3{t, 0, 1}, Vec3{-t, 0, -1}, Vec3{-t, 0, 1}})
            s.verts.push_back(math::normalize(v));
        s.tris = {0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
                  3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1};
        for (int level = 0; level < 2; ++level) {
            std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> mid;
            const auto midpoint = [&](std::uint32_t a, std::uint32_t b) {
                const auto key = std::minmax(a, b);
                if (auto it = mid.find(key); it != mid.end()) return it->second;
                const auto id = static_cast<std::uint32_t>(s.verts.size());
                s.verts.push_back(math::normalize((s.verts[a] + s.verts[b]) * 0.5f));
                mid.emplace(key, id);
                return id;
            };
            std::vector<std::uint32_t> next;
            next.reserve(s.tris.size() * 4);
            for (std::size_t i = 0; i < s.tris.size(); i += 3) {
                const std::uint32_t a = s.tris[i], b = s.tris[i + 1], c = s.tris[i + 2];
                const std::uint32_t ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
                next.insert(next.end(), {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca});
            }
            s.tris = std::move(next);
        }
        return s;
    }();
    return sphere;
}

// Smooth value noise on a lattice, for lumpy leaf masses that stay seamless across vertices.
float lattice(std::uint32_t seed, int x, int y, int z) {
    return rnd(seed, static_cast<std::uint32_t>(x) * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u ^ static_cast<std::uint32_t>(z) * 83492791u);
}
float noise3(std::uint32_t seed, Vec3 p) {
    const float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const int x = static_cast<int>(fx), y = static_cast<int>(fy), z = static_cast<int>(fz);
    const auto s = [](float t) { return t * t * (3.0f - 2.0f * t); };
    const float u = s(p.x - fx), v = s(p.y - fy), w = s(p.z - fz);
    const auto l = [&](float a, float b, float t) { return a + (b - a) * t; };
    return l(l(l(lattice(seed, x, y, z), lattice(seed, x + 1, y, z), u), l(lattice(seed, x, y + 1, z), lattice(seed, x + 1, y + 1, z), u), v),
             l(l(lattice(seed, x, y, z + 1), lattice(seed, x + 1, y, z + 1), u), l(lattice(seed, x, y + 1, z + 1), lattice(seed, x + 1, y + 1, z + 1), u), v), w);
}

struct Mesh {
    scene::ProceduralMesh& m;
    std::uint32_t vertex(Vec3 p, Vec3 n, std::uint32_t color) {
        m.positions.push_back(p);
        m.normals.push_back(n);
        m.colors.push_back(color);
        return static_cast<std::uint32_t>(m.positions.size() - 1);
    }
    void tri(std::uint32_t a, std::uint32_t b, std::uint32_t c) { m.indices.insert(m.indices.end(), {a, b, c}); }

    // Tapered tube along a polyline: `rings` centres and radii, `sides` around, smooth normals,
    // colour from `col(t)` along its length. Open at both ends (the base sits in the ground,
    // the tip inside the crown).
    void tube(const std::vector<Vec3>& centres, const std::vector<float>& radii, int sides, math::Color bark, float base_shade) {
        const auto first = static_cast<std::uint32_t>(m.positions.size());
        const std::size_t n = centres.size();
        for (std::size_t j = 0; j < n; ++j) {
            const Vec3 axis = math::normalize(centres[std::min(j + 1, n - 1)] - centres[j > 0 ? j - 1 : 0]);
            const Vec3 ref = std::abs(axis.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
            const Vec3 u = math::normalize(math::cross(ref, axis)), v = math::cross(axis, u);
            const float t = static_cast<float>(j) / static_cast<float>(n - 1);
            const std::uint32_t color = pack(bark, base_shade + (1.0f - base_shade) * std::min(1.0f, t * 2.0f));
            for (int i = 0; i < sides; ++i) {
                const float a = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(sides);
                const Vec3 d = u * std::cos(a) + v * std::sin(a);
                vertex(centres[j] + d * radii[j], d, color);
            }
        }
        for (std::size_t j = 0; j + 1 < n; ++j)
            for (int i = 0; i < sides; ++i) {
                const auto at = [&](std::size_t r, int k) { return first + static_cast<std::uint32_t>(r * static_cast<std::size_t>(sides) + static_cast<std::size_t>(k % sides)); };
                tri(at(j, i), at(j + 1, i + 1), at(j + 1, i));
                tri(at(j, i), at(j, i + 1), at(j + 1, i + 1));
            }
    }
};

// One leaf mass of a broadleaf crown: a noise-displaced icosphere. Normals lean towards the
// direction from the crown centre, so the whole crown shades as one soft volume rather than as
// separate balls; the underside and the inner faces are darker (cheap ambient occlusion).
void leaf_mass(Mesh& out, Vec3 c, float r, Vec3 crown_centre, float crown_r, float crown_lo, float crown_hi, math::Color leaf,
               float hue, std::uint32_t seed) {
    const Icosphere& s = icosphere();
    const auto first = static_cast<std::uint32_t>(out.m.positions.size());
    for (const Vec3 d : s.verts) {
        const float lump = 0.82f + 0.3f * noise3(seed, d * 2.2f + Vec3{c.x, c.y, c.z}) + 0.1f * noise3(seed + 7u, d * 6.0f);
        const Vec3 p = c + d * (r * lump);
        const Vec3 radial = math::normalize(p - crown_centre + Vec3{0, 0.25f * crown_r, 0});
        const Vec3 n = math::normalize(radial * 0.65f + d * 0.35f);
        const float up = std::clamp((p.y - crown_lo) / std::max(crown_hi - crown_lo, 0.01f), 0.0f, 1.0f);
        const float outer = std::clamp(math::length(p - crown_centre) / std::max(crown_r, 0.01f), 0.0f, 1.0f);
        const float shade = (0.42f + 0.5f * up) * (0.7f + 0.3f * outer) * (0.9f + 0.2f * std::max(0.0f, d.y));
        out.vertex(p, n, pack(leaf, shade * (0.92f + 0.16f * hue), hue * 2.0f - 1.0f));
    }
    for (std::size_t i = 0; i < s.tris.size(); i += 3) out.tri(first + s.tris[i], first + s.tris[i + 1], first + s.tris[i + 2]);
}

void broadleaf(const TreeSettings& st, TreeMeshes& out) {
    const std::uint32_t seed = st.seed;
    const float H = std::max(st.height, 0.5f), R = std::max(st.crown_radius, 0.2f);
    const float trunk_r = std::clamp(0.045f * H, 0.05f, 0.6f);
    const float lean_a = rnd(seed, 1) * 2.0f * kPi, lean = (0.04f + 0.06f * rnd(seed, 2)) * H;
    const Vec3 lean_dir{std::cos(lean_a), 0, std::sin(lean_a)};
    const float crown_lo = 0.42f * H, crown_hi = H;
    const Vec3 crown_centre = Vec3{0, (crown_lo + crown_hi) * 0.5f + 0.05f * H, 0} + lean_dir * lean;
    const auto trunk_at = [&](float t) { return Vec3{0, t * 0.78f * H, 0} + lean_dir * (lean * t * t); };

    Mesh trunk{out.trunk};
    {
        std::vector<Vec3> c;
        std::vector<float> r;
        for (int j = 0; j <= 6; ++j) {
            const float t = static_cast<float>(j) / 6.0f;
            c.push_back(trunk_at(t) + Vec3{0, j == 0 ? -0.2f : 0.0f, 0});
            r.push_back(trunk_r * (1.0f - 0.55f * t) * (1.0f + 0.5f * std::max(0.0f, 1.0f - t * 6.0f))); // flared root
        }
        trunk.tube(c, r, 9, st.bark_color, 0.6f);
    }

    // Leaf masses: one on top, a ring of 5-7 around and below it, each with a branch from the
    // upper trunk so the crown reads as held up rather than floating.
    Mesh crown{out.crown};
    const int ring = 5 + static_cast<int>(rnd(seed, 3) * 3.0f);
    leaf_mass(crown, crown_centre + Vec3{0, 0.18f * (crown_hi - crown_lo), 0}, 0.62f * R, crown_centre, R, crown_lo, crown_hi, st.leaf_color,
              rnd(seed, 4), seed * 31u + 1u);
    for (int k = 0; k < ring; ++k) {
        const float a = lean_a + 2.0f * kPi * (static_cast<float>(k) + 0.35f * rnd(seed, 10u + static_cast<std::uint32_t>(k))) / static_cast<float>(ring);
        const float dist = R * (0.5f + 0.2f * rnd(seed, 20u + static_cast<std::uint32_t>(k)));
        const float y = crown_lo + (crown_hi - crown_lo) * (0.25f + 0.35f * rnd(seed, 30u + static_cast<std::uint32_t>(k)));
        const Vec3 c = Vec3{std::cos(a) * dist, y, std::sin(a) * dist} + lean_dir * lean;
        const float r = R * (0.46f + 0.16f * rnd(seed, 40u + static_cast<std::uint32_t>(k)));
        leaf_mass(crown, c, r, crown_centre, R, crown_lo, crown_hi, st.leaf_color, rnd(seed, 50u + static_cast<std::uint32_t>(k)),
                  seed * 31u + 2u + static_cast<std::uint32_t>(k));
        if (k % 2 == 0 || ring <= 5) {
            const Vec3 from = trunk_at(0.55f + 0.2f * rnd(seed, 60u + static_cast<std::uint32_t>(k)));
            const Vec3 to = c * 0.8f + from * 0.2f;
            const Vec3 midp = (from + to) * 0.5f + Vec3{0, 0.12f * math::length(to - from), 0};
            trunk.tube({from, midp, to}, {trunk_r * 0.4f, trunk_r * 0.3f, trunk_r * 0.16f}, 6, st.bark_color, 1.0f);
        }
    }
}

void conifer(const TreeSettings& st, TreeMeshes& out) {
    const std::uint32_t seed = st.seed;
    const float H = std::max(st.height, 0.5f), R = std::max(st.crown_radius, 0.2f);
    const float trunk_r = std::clamp(0.03f * H, 0.04f, 0.5f);
    Mesh trunk{out.trunk};
    trunk.tube({{0, -0.2f, 0}, {0, 0.3f * H, 0}, {0, 0.95f * H, 0}}, {trunk_r * 1.3f, trunk_r, trunk_r * 0.3f}, 8, st.bark_color, 0.6f);

    // Overlapping drooping skirts, widest at the bottom. Each skirt is a cone whose rim zig-zags
    // between long and short boughs and droops at the tips, closed underneath by a shallow cone.
    Mesh crown{out.crown};
    const int tiers = 5 + static_cast<int>(rnd(seed, 1) * 2.0f);
    const float lo = 0.16f * H;
    constexpr int kTips = 11;
    for (int t = 0; t < tiers; ++t) {
        const float f = static_cast<float>(t) / static_cast<float>(tiers - 1); // 0 bottom, 1 top
        const float base_y = lo + (H * 0.86f - lo) * f;
        const float top_y = std::min(base_y + H * (0.34f - 0.12f * f), H);
        const float r = R * (1.0f - 0.78f * f) * (0.92f + 0.16f * rnd(seed, 10u + static_cast<std::uint32_t>(t)));
        const float droop = 0.22f * r;
        const float spin = rnd(seed, 20u + static_cast<std::uint32_t>(t)) * 2.0f * kPi;
        const float hue = rnd(seed, 30u + static_cast<std::uint32_t>(t));
        const std::uint32_t apex_c = pack(st.leaf_color, 0.95f + 0.1f * hue, hue * 2.0f - 1.0f);
        const std::uint32_t apex = crown.vertex({0, top_y, 0}, {0, 1, 0}, apex_c);
        const std::uint32_t under = crown.vertex({0, base_y + 0.08f * r, 0}, {0, -1, 0}, pack(st.leaf_color, 0.3f));
        std::array<std::uint32_t, 2 * kTips> rim{};
        std::array<std::uint32_t, 2 * kTips> rim_under{};
        for (int i = 0; i < 2 * kTips; ++i) {
            const float a = spin + kPi * static_cast<float>(i) / kTips;
            const bool tip = (i & 1) == 0;
            const float rr = r * (tip ? 1.0f : 0.72f) * (0.9f + 0.2f * rnd(seed, 100u + static_cast<std::uint32_t>(t * 64 + i)));
            const Vec3 p{std::cos(a) * rr, base_y - (tip ? droop : droop * 0.3f), std::sin(a) * rr};
            const Vec3 n = math::normalize(Vec3{std::cos(a), 0.9f, std::sin(a)});
            const float shade = (0.45f + 0.35f * f) * (tip ? 1.0f : 0.8f);
            rim[static_cast<std::size_t>(i)] = crown.vertex(p, n, pack(st.leaf_color, shade * (0.92f + 0.16f * hue), hue * 2.0f - 1.0f));
            rim_under[static_cast<std::size_t>(i)] = crown.vertex(p, math::normalize(Vec3{std::cos(a) * 0.4f, -1, std::sin(a) * 0.4f}), pack(st.leaf_color, 0.3f));
        }
        for (int i = 0; i < 2 * kTips; ++i) {
            const auto a = static_cast<std::size_t>(i), b = static_cast<std::size_t>((i + 1) % (2 * kTips));
            crown.tri(apex, rim[b], rim[a]);
            crown.tri(under, rim_under[a], rim_under[b]);
        }
    }
}

std::uint64_t settings_hash(const TreeComponent& c) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    const auto mix = [&](const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    const auto kind = static_cast<std::uint8_t>(c.kind);
    const std::uint8_t shadows = c.cast_shadows ? 1 : 0;
    mix(&c.seed, sizeof(c.seed));
    mix(&kind, 1);
    mix(&c.height, sizeof(float));
    mix(&c.crown_radius, sizeof(float));
    for (const float f : {c.leaf_color.r, c.leaf_color.g, c.leaf_color.b, c.bark_color.r, c.bark_color.g, c.bark_color.b}) mix(&f, sizeof(f));
    for (const std::uint64_t u : {c.leaf_material.hi, c.leaf_material.lo, c.bark_material.hi, c.bark_material.lo}) mix(&u, sizeof(u));
    mix(&shadows, 1);
    return h | 1u; // never 0 (0 = not built)
}

void register_schema() {
    static std::once_flag once;
    std::call_once(once, [] {
        using C = TreeComponent;
        scene::Schema<C>("Tree", "World")
            .field(&C::seed, "seed", {.tooltip = "Same seed, same tree"})
            .enumeration(&C::kind, "kind", {"broadleaf", "conifer"})
            .field(&C::height, "height", {.min = 0.5f, .max = 60, .tooltip = "m"})
            .field(&C::crown_radius, "crown_radius", {.min = 0.2f, .max = 30, .tooltip = "m"})
            .field(&C::leaf_color, "leaf_color")
            .field(&C::bark_color, "bark_color")
            .field(&C::leaf_material, "leaf_material", {.tooltip = "Crown material; the starter leaves material sways in the wind", .hint = "material"})
            .field(&C::bark_material, "bark_material", {.tooltip = "Trunk and branch material", .hint = "material"})
            .field(&C::cast_shadows, "cast_shadows")
            .commit();
    });
}

} // namespace

TreeSettings TreeComponent::settings() const {
    TreeSettings s;
    s.seed = seed;
    s.kind = kind;
    s.height = height;
    s.crown_radius = crown_radius;
    s.leaf_color = leaf_color;
    s.bark_color = bark_color;
    return s;
}

TreeMeshes build_tree_meshes(const TreeSettings& settings) {
    TreeMeshes out;
    if (settings.kind == TreeKind::Conifer) conifer(settings, out);
    else broadleaf(settings, out);
    return out;
}

std::uint32_t build_trees(scene::Scene& scene) {
    register_schema();
    auto& w = scene.world();
    if (w.count<TreeComponent>() == 0) return 0;
    std::vector<Entity> stale;
    w.each<TreeComponent>([&](Entity e, TreeComponent& c) {
        if (c.built != settings_hash(c)) stale.push_back(e);
    });
    if (stale.empty()) return 0;
    VICTUS_PROFILE_SCOPE("procgen.build_trees");
    std::uint32_t built = 0;
    for (const Entity owner : stale) {
        for (const Entity child : scene.children(owner))
            if (w.get<scene::TransientComponent>(child)) scene.destroy(child);
        auto* c = w.get<TreeComponent>(owner);
        if (!c) continue;
        c->built = settings_hash(*c);
        TreeMeshes meshes = build_tree_meshes(c->settings());
        const struct {
            scene::ProceduralMesh* mesh;
            const char* name;
            Uuid material;
            float roughness;
        } parts[] = {{&meshes.trunk, "Tree Trunk", c->bark_material, 0.9f}, {&meshes.crown, "Tree Crown", c->leaf_material, 0.75f}};
        const bool shadows = c->cast_shadows;
        for (const auto& part : parts) {
            auto mesh = scene::publish_mesh(std::move(*part.mesh));
            if (!mesh) {
                VICTUS_LOG_WARN("procgen", "tree mesh: {}", mesh.error().message);
                continue;
            }
            const Entity e = scene.create(part.name, owner);
            w.add<scene::TransientComponent>(e);
            auto& mr = w.add<scene::MeshRendererComponent>(e);
            mr.material_asset = part.material;
            mr.roughness = part.roughness;
            mr.cast_shadows = shadows;
            w.add<scene::ProceduralMeshComponent>(e).mesh = std::move(*mesh);
        }
        ++built;
    }
    return built;
}

void register_tree_components() { register_schema(); }

} // namespace victus::procgen
