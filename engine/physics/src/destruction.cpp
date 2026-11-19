// SPDX-License-Identifier: Apache-2.0
// Destruction stage 1 (ADR-0093): deterministic Voronoi fracture of convex volumes and the runtime
// that swaps a Destructible for its fragments, frees pieces around impacts and drops unsupported
// islands (connection graph + base anchors, after Chaos Destruction / NVIDIA Blast).
#include "internal.hpp"

#include "victus/core/hash.hpp"
#include "victus/core/log.hpp"
#include "victus/core/profiler.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <unordered_set>

namespace victus::physics {

using math::Vec3;

namespace {

struct Face {
    std::vector<Vec3> v; // counter-clockwise seen from outside
    Vec3 n;
    std::int32_t tag{-1}; // -1 = exterior surface, else the Voronoi cell on the other side
};
using Poly = std::vector<Face>;

struct Plane {
    Vec3 n;
    float d{0}; // inside: dot(n, x) <= d
};

// Monotonic pseudo-angle in [0, 4): orders points around an axis with plain IEEE arithmetic, so
// the fracture is bit-identical on every platform (no libm trigonometry).
float pseudo_angle(float x, float y) {
    const float s = std::abs(x) + std::abs(y);
    if (s <= 0.0f) return 0.0f;
    const float a = x / s; // [-1, 1]
    return y >= 0.0f ? 1.0f - a : 3.0f + a;
}

void basis(Vec3 n, Vec3& u, Vec3& w) {
    u = math::normalize(math::cross(n, std::abs(n.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0}));
    w = math::cross(n, u);
}

float polygon_area(const std::vector<Vec3>& v) {
    Vec3 a{};
    for (std::size_t i = 1; i + 1 < v.size(); ++i) a += math::cross(v[i] - v[0], v[i + 1] - v[0]);
    return 0.5f * math::length(a);
}

void dedupe(std::vector<Vec3>& pts, float eps) {
    std::vector<Vec3> out;
    out.reserve(pts.size());
    for (const Vec3& p : pts) {
        bool dup = false;
        for (const Vec3& q : out)
            if (math::length(p - q) <= eps) {
                dup = true;
                break;
            }
        if (!dup) out.push_back(p);
    }
    pts.swap(out);
}

// Keeps the part of a convex polyhedron with dot(n, x) <= d, closing the cut with a face tagged `tag`.
Poly clip(const Poly& poly, const Plane& pl, std::int32_t tag, float eps) {
    bool any_out = false, any_in = false;
    for (const Face& f : poly)
        for (const Vec3& p : f.v) {
            const float s = math::dot(pl.n, p) - pl.d;
            any_out |= s > eps;
            any_in |= s < -eps;
        }
    if (!any_out) return poly;
    if (!any_in) return {};
    Poly out;
    out.reserve(poly.size() + 1);
    std::vector<Vec3> cap;
    for (const Face& f : poly) {
        Face r;
        r.n = f.n;
        r.tag = f.tag;
        const std::size_t n = f.v.size();
        for (std::size_t i = 0; i < n; ++i) {
            const Vec3 a = f.v[i], b = f.v[(i + 1) % n];
            const float sa = math::dot(pl.n, a) - pl.d, sb = math::dot(pl.n, b) - pl.d;
            const bool ina = sa <= eps, inb = sb <= eps;
            if (ina) {
                r.v.push_back(a);
                if (std::abs(sa) <= eps) cap.push_back(a);
            }
            if (ina != inb && std::abs(sa) > eps && std::abs(sb) > eps) {
                const Vec3 p = a + (b - a) * (sa / (sa - sb));
                r.v.push_back(p);
                cap.push_back(p);
            }
        }
        dedupe(r.v, eps);
        if (r.v.size() >= 3 && polygon_area(r.v) > eps * eps) out.push_back(std::move(r));
    }
    dedupe(cap, eps);
    if (cap.size() >= 3) {
        Vec3 c{};
        for (const Vec3& p : cap) c += p;
        c = c / static_cast<float>(cap.size());
        Vec3 u, w;
        basis(pl.n, u, w);
        std::sort(cap.begin(), cap.end(), [&](Vec3 p, Vec3 q) {
            return pseudo_angle(math::dot(p - c, u), math::dot(p - c, w)) <
                   pseudo_angle(math::dot(q - c, u), math::dot(q - c, w));
        });
        if (polygon_area(cap) > eps * eps) out.push_back({std::move(cap), pl.n, tag});
    }
    return out;
}

void volume_and_centroid(const Poly& poly, float& volume, Vec3& centroid) {
    Vec3 c0{};
    std::size_t count = 0;
    for (const Face& f : poly)
        for (const Vec3& p : f.v) c0 += p, ++count;
    if (count == 0) {
        volume = 0;
        centroid = {};
        return;
    }
    c0 = c0 / static_cast<float>(count);
    double vol = 0;
    Vec3 acc{};
    for (const Face& f : poly)
        for (std::size_t i = 1; i + 1 < f.v.size(); ++i) {
            const Vec3 a = f.v[0] - c0, b = f.v[i] - c0, c = f.v[i + 1] - c0;
            const float v = math::dot(a, math::cross(b, c)) / 6.0f;
            vol += v;
            acc += (a + b + c) * (v * 0.25f);
        }
    volume = static_cast<float>(vol);
    centroid = vol > 0 ? c0 + acc / static_cast<float>(vol) : c0;
}

float max_distance(const Poly& poly, Vec3 from) {
    float r = 0;
    for (const Face& f : poly)
        for (const Vec3& p : f.v) r = std::max(r, math::length(p - from));
    return r;
}

// Planes of a convex triangle mesh, or nothing when it is concave or inconsistently wound.
std::vector<Plane> mesh_planes(const std::vector<Vec3>& pts, const std::vector<std::uint32_t>& idx, float tol) {
    std::vector<Plane> planes;
    if (idx.empty() || idx.size() % 3 != 0 || idx.size() / 3 > 2048) return {};
    for (std::size_t t = 0; t < idx.size(); t += 3) {
        if (idx[t] >= pts.size() || idx[t + 1] >= pts.size() || idx[t + 2] >= pts.size()) return {};
        const Vec3 a = pts[idx[t]], b = pts[idx[t + 1]], c = pts[idx[t + 2]];
        const Vec3 cr = math::cross(b - a, c - a);
        const float len = math::length(cr);
        if (len <= tol * tol) continue;
        const Vec3 n = cr / len;
        const float d = math::dot(n, a);
        bool dup = false;
        for (const Plane& p : planes)
            if (math::dot(p.n, n) > 1.0f - 1e-5f && std::abs(p.d - d) <= tol) {
                dup = true;
                break;
            }
        if (dup) continue;
        for (const Vec3& p : pts)
            if (math::dot(n, p) - d > tol) return {}; // concave
        planes.push_back({n, d});
    }
    return planes.size() >= 4 ? planes : std::vector<Plane>{};
}

// Discrete-orientation polytope: the axes plus a Fibonacci sphere of directions.
std::vector<Plane> dop_planes(const std::vector<Vec3>& pts) {
    std::vector<Vec3> dirs{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    constexpr int kCount = 84;
    const float golden = std::numbers::pi_v<float> * (3.0f - std::sqrt(5.0f));
    for (int i = 0; i < kCount; ++i) {
        const float y = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / kCount;
        const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const float a = golden * static_cast<float>(i);
        dirs.push_back({std::cos(a) * r, y, std::sin(a) * r});
    }
    std::vector<Plane> planes;
    for (const Vec3& d : dirs) {
        float m = -std::numeric_limits<float>::max();
        for (const Vec3& p : pts) m = std::max(m, math::dot(d, p));
        planes.push_back({d, m});
    }
    return planes;
}

Poly box_poly(const math::Aabb& b) {
    const Vec3 l = b.min, h = b.max;
    const Vec3 c[8] = {{l.x, l.y, l.z}, {h.x, l.y, l.z}, {h.x, h.y, l.z}, {l.x, h.y, l.z},
                       {l.x, l.y, h.z}, {h.x, l.y, h.z}, {h.x, h.y, h.z}, {l.x, h.y, h.z}};
    return {
        {{c[0], c[3], c[2], c[1]}, {0, 0, -1}, -1}, {{c[4], c[5], c[6], c[7]}, {0, 0, 1}, -1},
        {{c[0], c[4], c[7], c[3]}, {-1, 0, 0}, -1}, {{c[1], c[2], c[6], c[5]}, {1, 0, 0}, -1},
        {{c[0], c[1], c[5], c[4]}, {0, -1, 0}, -1}, {{c[3], c[7], c[6], c[2]}, {0, 1, 0}, -1},
    };
}

FracturePiece to_piece(const Poly& poly) {
    FracturePiece out;
    volume_and_centroid(poly, out.volume, out.centroid);
    for (const Face& f : poly) {
        const auto base = static_cast<std::uint32_t>(out.positions.size());
        // Planar projection on the dominant axis, 1 texture repeat per metre.
        const Vec3 an = detail::abs(f.n);
        const int drop = an.x >= an.y && an.x >= an.z ? 0 : (an.y >= an.z ? 1 : 2);
        const std::uint32_t color = f.tag < 0 ? 0xFFFFFFFFu : 0xFF9A9A9Au;
        for (const Vec3& p : f.v) {
            out.positions.push_back(p);
            out.normals.push_back(f.n);
            out.uvs.push_back(drop == 0 ? math::Vec2{p.z, p.y} : drop == 1 ? math::Vec2{p.x, p.z} : math::Vec2{p.x, p.y});
            out.colors.push_back(color);
            out.bounds.expand(p);
        }
        for (std::uint32_t i = 1; i + 1 < f.v.size(); ++i) {
            out.indices.push_back(base);
            out.indices.push_back(base + i);
            out.indices.push_back(base + i + 1);
        }
    }
    return out;
}

} // namespace

Result<Fracture> fracture_convex(const std::vector<Vec3>& points, const std::vector<std::uint32_t>& indices,
                                 const FractureSettings& settings) {
    math::Aabb bounds;
    for (const Vec3& p : points) bounds.expand(p);
    if (points.size() < 4 || !bounds.valid()) return Error{ErrorCode::InvalidArgument, "fracture: needs at least four points"};
    const Vec3 ext = bounds.max - bounds.min;
    const float scale = std::max({ext.x, ext.y, ext.z});
    if (!(scale > 0.0f) || std::min({ext.x, ext.y, ext.z}) <= scale * 1e-4f)
        return Error{ErrorCode::InvalidArgument, "fracture: the points are (nearly) coplanar"};
    const float eps = scale * 1e-5f;

    std::vector<Plane> planes = mesh_planes(points, indices, scale * 1e-4f);
    if (planes.empty()) planes = dop_planes(points);
    math::Aabb grown = bounds;
    grown.min -= ext * 0.01f + Vec3{eps, eps, eps};
    grown.max += ext * 0.01f + Vec3{eps, eps, eps};
    Poly hull = box_poly(grown);
    for (const Plane& p : planes) {
        hull = clip(hull, p, -1, eps);
        if (hull.empty()) return Error{ErrorCode::InvalidArgument, "fracture: degenerate hull"};
    }
    Fracture result;
    Vec3 hull_centroid;
    volume_and_centroid(hull, result.volume, hull_centroid);
    if (!(result.volume > 0.0f)) return Error{ErrorCode::InvalidArgument, "fracture: hull has no volume"};

    // Seeds: rejection-sampled inside the hull with a deterministic PCG stream.
    const std::uint32_t wanted = std::clamp<std::uint32_t>(settings.pieces, 1u, 256u);
    std::vector<Vec3> seeds;
    Random rng(0x9e3779b97f4a7c15ull ^ settings.seed, 0x5DE57u);
    const float spacing = scale * 1e-3f;
    for (std::uint32_t attempt = 0; seeds.size() < wanted && attempt < wanted * 400u; ++attempt) {
        const Vec3 p{rng.range_float(bounds.min.x, bounds.max.x), rng.range_float(bounds.min.y, bounds.max.y),
                     rng.range_float(bounds.min.z, bounds.max.z)};
        bool inside = true;
        for (const Plane& pl : planes)
            if (math::dot(pl.n, p) > pl.d - spacing) {
                inside = false;
                break;
            }
        for (const Vec3& s : seeds)
            if (inside && math::length(s - p) < spacing) inside = false;
        if (inside) seeds.push_back(p);
    }
    if (seeds.empty()) seeds.push_back(hull_centroid);

    std::vector<Poly> cells(seeds.size());
    std::vector<std::uint32_t> order(seeds.size());
    for (std::size_t i = 0; i < seeds.size(); ++i) {
        for (std::uint32_t j = 0; j < order.size(); ++j) order[j] = j;
        std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
            const float da = math::length(seeds[a] - seeds[i]), db = math::length(seeds[b] - seeds[i]);
            return da != db ? da < db : a < b;
        });
        Poly cell = hull;
        float reach = max_distance(cell, seeds[i]);
        for (std::uint32_t j : order) {
            if (j == i) continue;
            const Vec3 delta = seeds[j] - seeds[i];
            const float dist = math::length(delta);
            if (dist * 0.5f >= reach) break; // every farther bisector misses the cell
            const Vec3 n = delta / dist;
            cell = clip(cell, {n, math::dot(n, (seeds[i] + seeds[j]) * 0.5f)}, static_cast<std::int32_t>(j), eps);
            if (cell.empty()) break;
            reach = max_distance(cell, seeds[i]);
        }
        cells[i] = std::move(cell);
    }

    // Drop empty cells and remap neighbour tags to piece indices.
    std::vector<std::int32_t> remap(cells.size(), -1);
    for (std::size_t i = 0; i < cells.size(); ++i)
        if (!cells[i].empty()) {
            remap[i] = static_cast<std::int32_t>(result.pieces.size());
            result.pieces.push_back(to_piece(cells[i]));
        }
    const float min_face = scale * scale * 1e-6f;
    for (std::size_t i = 0; i < cells.size(); ++i) {
        if (remap[i] < 0) continue;
        for (const Face& f : cells[i]) {
            if (f.tag < 0 || remap[static_cast<std::size_t>(f.tag)] < 0 || polygon_area(f.v) <= min_face) continue;
            const auto a = static_cast<std::uint32_t>(remap[i]);
            const auto b = static_cast<std::uint32_t>(remap[static_cast<std::size_t>(f.tag)]);
            result.pieces[a].neighbours.push_back(b);
            result.pieces[b].neighbours.push_back(a);
        }
    }
    for (FracturePiece& p : result.pieces) {
        std::sort(p.neighbours.begin(), p.neighbours.end());
        p.neighbours.erase(std::unique(p.neighbours.begin(), p.neighbours.end()), p.neighbours.end());
    }
    return result;
}

// ---- runtime -------------------------------------------------------------------------------------

namespace detail {

namespace {

constexpr float kMinFragmentMass = 0.01f;

void shape_points(const ColliderComponent& col, std::vector<Vec3>& pts) {
    const Vec3 o = col.offset;
    switch (col.shape) {
    case ColliderComponent::Shape::Box: {
        const Vec3 h = col.size * 0.5f;
        for (int i = 0; i < 8; ++i)
            pts.push_back(o + Vec3{(i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z});
        break;
    }
    case ColliderComponent::Shape::Sphere:
    case ColliderComponent::Shape::Capsule:
    case ColliderComponent::Shape::Cylinder: {
        const float r = col.radius;
        const bool sphere = col.shape == ColliderComponent::Shape::Sphere;
        const float half = sphere ? 0.0f : std::max(0.0f, col.height * 0.5f - (col.shape == ColliderComponent::Shape::Capsule ? r : 0.0f));
        constexpr int kRings = 7, kSegments = 16;
        for (int ring = 0; ring <= kRings; ++ring) {
            const float t = static_cast<float>(ring) / kRings; // 0..1 bottom to top
            float y, rr;
            if (col.shape == ColliderComponent::Shape::Cylinder) {
                y = (t - 0.5f) * col.height;
                rr = r;
            } else {
                const float a = (t - 0.5f) * std::numbers::pi_v<float>;
                y = std::sin(a) * r + (t < 0.5f ? -half : half);
                rr = std::cos(a) * r;
            }
            for (int s = 0; s < kSegments; ++s) {
                const float a = 2.0f * std::numbers::pi_v<float> * static_cast<float>(s) / kSegments;
                pts.push_back(o + Vec3{std::cos(a) * rr, y, std::sin(a) * rr});
            }
        }
        break;
    }
    default: break;
    }
}

std::uint64_t fingerprint(const std::vector<Vec3>& pts, const std::vector<std::uint32_t>& idx, const DestructibleComponent& d) {
    std::uint64_t h = fnv1a64(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(pts.data()), pts.size() * sizeof(Vec3)));
    h = fnv1a64(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(idx.data()), idx.size() * sizeof(std::uint32_t)), h);
    const std::uint32_t s[2] = {d.pieces, d.seed};
    return fnv1a64(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(s), sizeof(s)), h);
}

Vec3 to_world(const Pose& p, Vec3 local) { return p.position + p.rotation.rotate(p.scale * local); }

struct Owned {
    Entity entity;
    DestructibleFragmentComponent* frag{nullptr};
    RigidBodyComponent* rb{nullptr};
};

std::vector<Owned> fragments_of(scene::Scene& scene, const Uuid& owner, std::size_t pieces) {
    std::vector<Owned> out(pieces);
    scene.world().each<DestructibleFragmentComponent>([&](Entity e, DestructibleFragmentComponent& f) {
        if (f.owner == owner && f.piece < pieces) out[f.piece] = {e, &f, scene.world().get<RigidBodyComponent>(e)};
    });
    return out;
}

} // namespace

const DestructionState::Cached* DestructionState::fracture_of(scene::Scene& scene, Entity owner, CollisionMeshCache& meshes) {
    auto* d = scene.world().get<DestructibleComponent>(owner);
    if (!d) return nullptr;
    std::vector<Vec3> pts;
    std::vector<std::uint32_t> idx;
    ColliderComponent probe;
    probe.shape = ColliderComponent::Shape::Convex;
    if (auto mesh = meshes.resolve(scene, owner, probe)) {
        pts = mesh->positions;
        idx = mesh->indices;
    } else if (const auto* col = scene.world().get<ColliderComponent>(owner)) {
        if (col->shape == ColliderComponent::Shape::Convex || col->shape == ColliderComponent::Shape::Mesh) {
            if (auto m = meshes.resolve(scene, owner, *col)) pts = m->positions, idx = m->indices;
        } else {
            shape_points(*col, pts);
        }
    }
    const Uuid id = scene.uuid(owner);
    const std::uint64_t key = fingerprint(pts, idx, *d);
    Cached& slot = cache[id];
    if (slot.key == key && slot.fracture) return &slot;
    auto made = fracture_convex(pts, idx, {d->pieces, d->seed});
    if (!made) {
        if (warned.insert(id).second)
            VICTUS_LOG_WARN("physics", "destructible '{}': {}", id.to_string(), made.error().message);
        cache.erase(id);
        return nullptr;
    }
    slot.key = key;
    slot.fracture = std::make_shared<const Fracture>(std::move(*made));
    slot.meshes.clear();
    for (const FracturePiece& p : slot.fracture->pieces) {
        scene::ProceduralMesh m;
        m.positions.reserve(p.positions.size());
        for (const Vec3& v : p.positions) m.positions.push_back(v - p.centroid);
        m.normals = p.normals;
        m.uvs = p.uvs;
        m.colors = p.colors;
        m.indices = p.indices;
        auto pub = scene::publish_mesh(std::move(m));
        slot.meshes.push_back(pub ? *pub : nullptr);
    }
    return &slot;
}

std::size_t restore_fragments(scene::Scene& scene, DestructionState& state, CollisionMeshCache& meshes) {
    std::vector<Entity> missing;
    scene.world().each<DestructibleFragmentComponent>([&](Entity e, DestructibleFragmentComponent&) {
        if (!scene.world().get<scene::ProceduralMeshComponent>(e)) missing.push_back(e);
    });
    std::size_t restored = 0;
    for (Entity e : missing) {
        const auto* f = scene.world().get<DestructibleFragmentComponent>(e);
        const Entity owner = scene.find(f->owner);
        if (owner.is_null()) continue;
        const auto* c = state.fracture_of(scene, owner, meshes);
        if (!c || f->piece >= c->meshes.size() || !c->meshes[f->piece]) continue;
        scene.world().add<scene::ProceduralMeshComponent>(e, scene::ProceduralMeshComponent{c->meshes[f->piece]});
        ++restored;
    }
    return restored;
}

namespace {

struct Freed {
    Entity e;
    Vec3 velocity;
    Vec3 angular;
};

void free_piece(Owned& o, const FracturePiece& piece, const DestructibleComponent& d, float mass_scale) {
    if (!o.rb || o.rb->type == BodyType::Dynamic) return;
    o.rb->type = BodyType::Dynamic;
    o.rb->mass = std::max(kMinFragmentMass, piece.volume * mass_scale * d.density);
    o.frag->age = 0.0f;
}

// Static fragments not connected through static neighbours to a fragment on the original's base.
void drop_unsupported(std::vector<Owned>& owned, const Fracture& fr, const DestructibleComponent& d, const Pose& pose,
                      float mass_scale, std::vector<Freed>& freed) {
    const std::size_t n = fr.pieces.size();
    std::vector<float> low(n, std::numeric_limits<float>::max());
    float base = std::numeric_limits<float>::max(), top = -base;
    for (std::size_t i = 0; i < n; ++i) {
        for (const Vec3& p : fr.pieces[i].positions) {
            const float y = to_world(pose, p).y;
            low[i] = std::min(low[i], y);
            top = std::max(top, y);
        }
        base = std::min(base, low[i]);
    }
    const float tol = std::max(0.005f, (top - base) * 0.02f);
    auto is_static = [&](std::size_t i) { return owned[i].rb && owned[i].rb->type != BodyType::Dynamic; };
    std::vector<char> reached(n, 0);
    std::vector<std::size_t> stack;
    for (std::size_t i = 0; i < n; ++i)
        if (is_static(i) && low[i] <= base + tol) reached[i] = 1, stack.push_back(i);
    while (!stack.empty()) {
        const std::size_t i = stack.back();
        stack.pop_back();
        for (std::uint32_t j : fr.pieces[i].neighbours)
            if (!reached[j] && is_static(j)) reached[j] = 1, stack.push_back(j);
    }
    for (std::size_t i = 0; i < n; ++i)
        if (is_static(i) && !reached[i]) {
            free_piece(owned[i], fr.pieces[i], d, mass_scale);
            freed.push_back({owned[i].entity, {}, {}});
        }
}

} // namespace

void destruction_step(scene::Scene& scene, PhysicsWorld& world, DestructionState& state, CollisionMeshCache& meshes,
                      const std::vector<ContactEvent>& events, float dt) {
    ecs::World& w = scene.world();
    auto owner_of = [&](Entity e, bool& fragment) -> Entity {
        fragment = false;
        if (e.is_null() || !w.alive(e)) return {};
        if (auto* d = w.get<DestructibleComponent>(e); d && !d->broken) return e;
        if (const auto* f = w.get<DestructibleFragmentComponent>(e)) {
            const auto* rb = w.get<RigidBodyComponent>(e);
            if (rb && rb->type == BodyType::Dynamic) return {}; // loose debris does not break further
            fragment = true;
            return scene.find(f->owner);
        }
        return {};
    };
    for (const ContactEvent& ev : events) {
        if (ev.kind != ContactEvent::Kind::Begin || ev.trigger || ev.is_2d || ev.impulse <= 0.0f) continue;
        bool fa = false, fb = false;
        const Entity oa = owner_of(ev.a, fa), ob = owner_of(ev.b, fb);
        // Pieces of one destructible (loose or not) do not damage each other: debris settling on
        // the standing part must not bring it down.
        const auto family = [&](Entity e) -> Uuid {
            if (e.is_null() || !w.alive(e)) return {};
            if (const auto* f = w.get<DestructibleFragmentComponent>(e)) return f->owner;
            return w.get<DestructibleComponent>(e) ? scene.uuid(e) : Uuid{};
        };
        const Uuid ua = family(ev.a);
        if (!ua.is_nil() && ua == family(ev.b)) continue;
        if (!oa.is_null()) state.pending.push_back({oa, ev.point, ev.impulse});
        if (!ob.is_null()) state.pending.push_back({ob, ev.point, ev.impulse});
    }

    std::vector<Freed> freed;
    std::vector<DestructionState::Damage> pending;
    pending.swap(state.pending);
    for (const DestructionState::Damage& hit : pending) {
        Entity e = hit.entity;
        if (!w.alive(e)) continue;
        if (const auto* f = w.get<DestructibleFragmentComponent>(e)) e = scene.find(f->owner);
        auto* d = e.is_null() ? nullptr : w.get<DestructibleComponent>(e);
        if (!d || hit.impulse < d->strength) continue;
        const auto* cached = state.fracture_of(scene, e, meshes);
        if (!cached) continue;
        const Fracture& fr = *cached->fracture;
        d = w.get<DestructibleComponent>(e); // fracture_of may have grown the component pools
        const Pose pose = world_pose(scene, e);
        const float mass_scale = std::abs(pose.scale.x * pose.scale.y * pose.scale.z);
        const Uuid id = scene.uuid(e);
        const auto* orb_ptr = w.get<RigidBodyComponent>(e);
        const std::optional<RigidBodyComponent> orb = orb_ptr ? std::optional<RigidBodyComponent>(*orb_ptr) : std::nullopt;
        const bool dynamic = orb && orb->type == BodyType::Dynamic;
        Vec3 v0{}, w0{};
        const bool just_broke = !d->broken;
        if (just_broke) {
            VICTUS_PROFILE_SCOPE("physics.destruction.break");
            if (dynamic && world.has_body(e)) v0 = world.linear_velocity(e), w0 = world.angular_velocity(e);
            const float owner_mass = orb ? orb->mass : 0.0f;
            scene::MeshRendererComponent look;
            if (auto* mr = w.get<scene::MeshRendererComponent>(e)) {
                look = *mr;
                mr->visible = false;
            }
            look.visible = true;
            ColliderComponent col;
            col.shape = ColliderComponent::Shape::Convex;
            if (const auto* oc = w.get<ColliderComponent>(e)) {
                col.friction = oc->friction;
                col.restitution = oc->restitution;
                col.layer = oc->layer;
                col.mask = oc->mask;
            }
            const DestructibleComponent settings = *d;
            const std::string base_name = [&] {
                const auto* nm = w.get<scene::Name>(e);
                return nm ? nm->value : std::string("Destructible");
            }();
            d->broken = true;
            for (std::size_t i = 0; i < fr.pieces.size(); ++i) {
                const FracturePiece& piece = fr.pieces[i];
                const Entity f = scene.create(base_name + " fragment " + std::to_string(i), e);
                w.get<scene::Transform>(f)->position = piece.centroid;
                w.add<scene::ProceduralMeshComponent>(f, scene::ProceduralMeshComponent{cached->meshes[i]});
                w.add<scene::MeshRendererComponent>(f, look);
                w.add<ColliderComponent>(f, col);
                RigidBodyComponent rb;
                rb.type = dynamic ? BodyType::Dynamic : BodyType::Static;
                rb.mass = dynamic && fr.volume > 0 ? std::max(kMinFragmentMass, owner_mass * piece.volume / fr.volume)
                                                   : std::max(kMinFragmentMass, piece.volume * mass_scale * settings.density);
                if (orb) {
                    rb.linear_damping = orb->linear_damping;
                    rb.angular_damping = orb->angular_damping;
                    rb.gravity_scale = orb->gravity_scale;
                }
                w.add<RigidBodyComponent>(f, rb);
                w.add<DestructibleFragmentComponent>(f, DestructibleFragmentComponent{id, static_cast<std::uint32_t>(i), 0.0f});
            }
            d = w.get<DestructibleComponent>(e);
        }
        std::vector<Owned> owned = fragments_of(scene, id, fr.pieces.size());
        auto burst = [&](std::size_t i, float dist) {
            const Vec3 c = to_world(pose, fr.pieces[i].centroid);
            const Vec3 away = c - hit.point;
            const float len = math::length(away);
            const float falloff = std::max(0.0f, 1.0f - dist / std::max(1e-3f, 2.0f * d->damage_radius));
            Vec3 v = v0 + math::cross(w0, c - pose.position);
            if (len > 1e-5f) v += away * (d->burst * falloff / len);
            return v;
        };
        if (dynamic) {
            if (!just_broke) continue; // its fragments are all loose already
            for (std::size_t i = 0; i < owned.size(); ++i)
                if (owned[i].rb)
                    freed.push_back({owned[i].entity, burst(i, math::length(to_world(pose, fr.pieces[i].centroid) - hit.point)), w0});
            continue;
        }
        // Static or kinematic original: free the pieces around the impact, then the islands it cut off.
        std::size_t nearest = owned.size();
        float nearest_dist = std::numeric_limits<float>::max();
        for (std::size_t i = 0; i < owned.size(); ++i) {
            if (!owned[i].rb || owned[i].rb->type == BodyType::Dynamic) continue;
            const FracturePiece& piece = fr.pieces[i];
            const float radius = math::length(piece.bounds.extents() * pose.scale);
            const float dist = std::max(0.0f, math::length(to_world(pose, piece.centroid) - hit.point) - radius);
            if (dist < nearest_dist) nearest_dist = dist, nearest = i;
            if (dist <= d->damage_radius) {
                free_piece(owned[i], piece, *d, mass_scale);
                freed.push_back({owned[i].entity, burst(i, dist), {}});
            }
        }
        if (nearest < owned.size() && owned[nearest].rb->type != BodyType::Dynamic) {
            free_piece(owned[nearest], fr.pieces[nearest], *d, mass_scale);
            freed.push_back({owned[nearest].entity, burst(nearest, nearest_dist), {}});
        }
        if (d->support) drop_unsupported(owned, fr, *d, pose, mass_scale, freed);
    }

    // Debris budget: age freed fragments, retire expired ones and the oldest beyond max_debris.
    struct Loose {
        Entity e;
        float age;
    };
    std::unordered_map<Uuid, std::vector<Loose>, UuidHash> loose;
    w.each<DestructibleFragmentComponent>([&](Entity e, DestructibleFragmentComponent& f) {
        const auto* rb = w.get<RigidBodyComponent>(e);
        if (!rb || rb->type != BodyType::Dynamic) return;
        f.age += dt;
        loose[f.owner].push_back({e, f.age});
    });
    std::vector<Entity> retire;
    for (auto& [owner, list] : loose) {
        const Entity oe = scene.find(owner);
        const auto* d = oe.is_null() ? nullptr : w.get<DestructibleComponent>(oe);
        const float lifetime = d ? d->debris_lifetime : 0.0f;
        const std::size_t cap = d ? d->max_debris : std::numeric_limits<std::uint32_t>::max();
        std::sort(list.begin(), list.end(), [](const Loose& a, const Loose& b) { return a.age > b.age; });
        for (std::size_t i = 0; i < list.size(); ++i)
            if ((lifetime > 0.0f && list[i].age > lifetime) || list.size() - i > cap) retire.push_back(list[i].e);
    }
    for (Entity e : retire)
        if (w.alive(e)) scene.destroy(e);

    if (!freed.empty()) {
        // Build the new bodies now so the freed pieces leave with their burst velocity this step.
        world.sync(scene, dt);
        for (const Freed& f : freed) {
            if (!w.alive(f.e) || !world.has_body(f.e)) continue;
            world.set_linear_velocity(f.e, f.velocity);
            world.set_angular_velocity(f.e, f.angular);
        }
    }
}

} // namespace detail

} // namespace victus::physics
