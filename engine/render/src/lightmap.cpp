// SPDX-License-Identifier: Apache-2.0
// Baked lighting (ADR-0122): xatlas unwrap, texel rasterisation, a SAH BVH and a CPU path tracer
// that bakes irradiance / pi into a lightmap atlas, then dilation and an edge-aware denoise.
#include "victus/render/lightmap.hpp"

#include "victus/core/jobs.hpp"
#include "victus/render/irradiance_volume.hpp"
#include "victus/core/profiler.hpp"

#include <xatlas.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>

namespace victus::render {

using math::Vec2;
using math::Vec3;
using math::Vec4;

namespace {

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

math::Mat4 normal_matrix(const math::Mat4& model) { return math::transpose(math::inverse(model)); }

float luminance(Vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// --- Ray tracing scene ---------------------------------------------------------------------

struct Tri {
    Vec3 v0, e1, e2; // world space
    Vec3 n;          // unit geometric normal (counter-clockwise front)
    std::uint32_t instance{0};
    std::uint32_t i0{0}, i1{0}, i2{0}; // source mesh vertex indices
};

struct BvhNode {
    Vec3 bmin, bmax;
    std::uint32_t first{0}; // leaf: first triangle; inner: right child (left child is this + 1)
    std::uint32_t count{0}; // triangles in a leaf, 0 for inner nodes
};

struct Hit {
    float t{std::numeric_limits<float>::max()};
    std::uint32_t tri{0};
    float u{0}, v{0};
    bool found{false};
};

struct Ray {
    Vec3 o, d, inv;
};

Ray make_ray(Vec3 o, Vec3 d) {
    auto inv = [](float x) { return std::fabs(x) > 1e-12f ? 1.0f / x : (x >= 0 ? 1e12f : -1e12f); };
    return {o, d, {inv(d.x), inv(d.y), inv(d.z)}};
}

bool slab(const Ray& r, Vec3 bmin, Vec3 bmax, float tmax, float& tnear) {
    float t0 = 0.0f, t1 = tmax;
    for (int a = 0; a < 3; ++a) {
        float tn = (bmin[a] - r.o[a]) * r.inv[a];
        float tf = (bmax[a] - r.o[a]) * r.inv[a];
        if (tn > tf) std::swap(tn, tf);
        t0 = std::max(t0, tn);
        t1 = std::min(t1, tf);
        if (t0 > t1) return false;
    }
    tnear = t0;
    return true;
}

// Möller–Trumbore, two-sided.
bool intersect(const Ray& r, const Tri& t, float tmax, float& out_t, float& out_u, float& out_v) {
    const Vec3 p = math::cross(r.d, t.e2);
    const float det = math::dot(t.e1, p);
    if (std::fabs(det) < 1e-12f) return false;
    const float inv = 1.0f / det;
    const Vec3 s = r.o - t.v0;
    const float u = math::dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    const Vec3 q = math::cross(s, t.e1);
    const float v = math::dot(r.d, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    const float tt = math::dot(t.e2, q) * inv;
    if (tt <= 0.0f || tt >= tmax) return false;
    out_t = tt;
    out_u = u;
    out_v = v;
    return true;
}

class Bvh {
public:
    void build(std::vector<Tri> tris) {
        tris_ = std::move(tris);
        nodes_.clear();
        if (tris_.empty()) return;
        order_.resize(tris_.size());
        centroids_.resize(tris_.size());
        for (std::uint32_t i = 0; i < tris_.size(); ++i) {
            order_[i] = i;
            const Tri& t = tris_[i];
            centroids_[i] = t.v0 + (t.e1 + t.e2) * (1.0f / 3.0f);
        }
        nodes_.reserve(tris_.size() * 2);
        nodes_.push_back({});
        subdivide(0, 0, static_cast<std::uint32_t>(tris_.size()));
        std::vector<Tri> sorted(tris_.size());
        for (std::size_t i = 0; i < order_.size(); ++i) sorted[i] = tris_[order_[i]];
        tris_ = std::move(sorted);
        order_.clear();
        centroids_.clear();
    }

    [[nodiscard]] const Tri& tri(std::uint32_t i) const { return tris_[i]; }
    [[nodiscard]] std::size_t size() const { return tris_.size(); }

    Hit closest(const Ray& r, float tmax) const {
        Hit h;
        h.t = tmax;
        if (nodes_.empty()) return h;
        std::uint32_t stack[64];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const BvhNode& n = nodes_[stack[--sp]];
            float tn = 0;
            if (!slab(r, n.bmin, n.bmax, h.t, tn)) continue;
            if (n.count > 0) {
                for (std::uint32_t i = n.first; i < n.first + n.count; ++i) {
                    float t = 0, u = 0, v = 0;
                    if (intersect(r, tris_[i], h.t, t, u, v)) {
                        h.t = t;
                        h.u = u;
                        h.v = v;
                        h.tri = i;
                        h.found = true;
                    }
                }
                continue;
            }
            const std::uint32_t self = static_cast<std::uint32_t>(&n - nodes_.data());
            const std::uint32_t l = self + 1, rr = n.first;
            float tl = 0, tr = 0;
            const bool hl = slab(r, nodes_[l].bmin, nodes_[l].bmax, h.t, tl);
            const bool hr = slab(r, nodes_[rr].bmin, nodes_[rr].bmax, h.t, tr);
            if (sp > 60) { // never expected (depth is bounded by the build), but stay safe
                if (hl) stack[sp++] = l;
                continue;
            }
            if (hl && hr) {
                if (tl < tr) {
                    stack[sp++] = rr;
                    stack[sp++] = l;
                } else {
                    stack[sp++] = l;
                    stack[sp++] = rr;
                }
            } else if (hl) {
                stack[sp++] = l;
            } else if (hr) {
                stack[sp++] = rr;
            }
        }
        return h;
    }

    bool occluded(const Ray& r, float tmax) const {
        if (nodes_.empty()) return false;
        std::uint32_t stack[64];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const BvhNode& n = nodes_[stack[--sp]];
            float tn = 0;
            if (!slab(r, n.bmin, n.bmax, tmax, tn)) continue;
            if (n.count > 0) {
                for (std::uint32_t i = n.first; i < n.first + n.count; ++i) {
                    float t = 0, u = 0, v = 0;
                    if (intersect(r, tris_[i], tmax, t, u, v)) return true;
                }
                continue;
            }
            const std::uint32_t self = static_cast<std::uint32_t>(&n - nodes_.data());
            if (sp > 61) continue;
            stack[sp++] = self + 1;
            stack[sp++] = n.first;
        }
        return false;
    }

private:
    void bounds_of(std::uint32_t first, std::uint32_t count, Vec3& bmin, Vec3& bmax) const {
        bmin = Vec3{std::numeric_limits<float>::max()};
        bmax = Vec3{-std::numeric_limits<float>::max()};
        for (std::uint32_t i = first; i < first + count; ++i) {
            const Tri& t = tris_[order_[i]];
            for (Vec3 p : {t.v0, t.v0 + t.e1, t.v0 + t.e2}) {
                bmin = math::min(bmin, p);
                bmax = math::max(bmax, p);
            }
        }
    }

    static float area(Vec3 bmin, Vec3 bmax) {
        const Vec3 e = bmax - bmin;
        return e.x < 0 ? 0.0f : 2.0f * (e.x * e.y + e.y * e.z + e.z * e.x);
    }

    void subdivide(std::uint32_t node, std::uint32_t first, std::uint32_t count, int depth = 0) {
        Vec3 bmin, bmax;
        bounds_of(first, count, bmin, bmax);
        nodes_[node].bmin = bmin;
        nodes_[node].bmax = bmax;
        auto make_leaf = [&] {
            nodes_[node].first = first;
            nodes_[node].count = count;
        };
        if (count <= 4 || depth >= 48) {
            make_leaf();
            return;
        }
        // Binned SAH over centroid bounds.
        Vec3 cmin{std::numeric_limits<float>::max()}, cmax{-std::numeric_limits<float>::max()};
        for (std::uint32_t i = first; i < first + count; ++i) {
            cmin = math::min(cmin, centroids_[order_[i]]);
            cmax = math::max(cmax, centroids_[order_[i]]);
        }
        constexpr int kBins = 12;
        float best_cost = std::numeric_limits<float>::max();
        int best_axis = -1, best_split = 0;
        for (int a = 0; a < 3; ++a) {
            const float extent = cmax[a] - cmin[a];
            if (extent <= 1e-9f) continue;
            struct Bin {
                Vec3 bmin{std::numeric_limits<float>::max()}, bmax{-std::numeric_limits<float>::max()};
                std::uint32_t n{0};
            };
            std::array<Bin, kBins> bins{};
            const float scale = static_cast<float>(kBins) / extent;
            for (std::uint32_t i = first; i < first + count; ++i) {
                const std::uint32_t ti = order_[i];
                const int b = std::min(kBins - 1, static_cast<int>((centroids_[ti][a] - cmin[a]) * scale));
                const Tri& t = tris_[ti];
                for (Vec3 p : {t.v0, t.v0 + t.e1, t.v0 + t.e2}) {
                    bins[b].bmin = math::min(bins[b].bmin, p);
                    bins[b].bmax = math::max(bins[b].bmax, p);
                }
                ++bins[b].n;
            }
            std::array<float, kBins - 1> left_area{}, right_area{};
            std::array<std::uint32_t, kBins - 1> left_n{}, right_n{};
            Vec3 lmin{std::numeric_limits<float>::max()}, lmax{-std::numeric_limits<float>::max()};
            Vec3 rmin = lmin, rmax = lmax;
            std::uint32_t ln = 0, rn = 0;
            for (int i = 0; i < kBins - 1; ++i) {
                ln += bins[i].n;
                lmin = math::min(lmin, bins[i].bmin);
                lmax = math::max(lmax, bins[i].bmax);
                left_n[i] = ln;
                left_area[i] = ln ? area(lmin, lmax) : 0.0f;
                const int j = kBins - 1 - i;
                rn += bins[j].n;
                rmin = math::min(rmin, bins[j].bmin);
                rmax = math::max(rmax, bins[j].bmax);
                right_n[j - 1] = rn;
                right_area[j - 1] = rn ? area(rmin, rmax) : 0.0f;
            }
            for (int i = 0; i < kBins - 1; ++i) {
                if (left_n[i] == 0 || right_n[i] == 0) continue;
                const float cost = left_area[i] * static_cast<float>(left_n[i]) + right_area[i] * static_cast<float>(right_n[i]);
                if (cost < best_cost) {
                    best_cost = cost;
                    best_axis = a;
                    best_split = i;
                }
            }
        }
        const float leaf_cost = area(bmin, bmax) * static_cast<float>(count);
        std::uint32_t mid = first;
        if (best_axis >= 0 && best_cost < leaf_cost) {
            const float scale = static_cast<float>(kBins) / (cmax[best_axis] - cmin[best_axis]);
            auto* begin = order_.data() + first;
            auto* end = begin + count;
            auto* m = std::stable_partition(begin, end, [&](std::uint32_t ti) {
                const int b = std::min(kBins - 1, static_cast<int>((centroids_[ti][best_axis] - cmin[best_axis]) * scale));
                return b <= best_split;
            });
            mid = static_cast<std::uint32_t>(m - order_.data());
        } else if (count <= 16) {
            make_leaf();
            return;
        }
        if (mid == first || mid == first + count) { // degenerate split: median on the widest axis
            int axis = 0;
            const Vec3 e = cmax - cmin;
            if (e.y > e[axis]) axis = 1;
            if (e.z > e[axis]) axis = 2;
            auto* begin = order_.data() + first;
            std::stable_sort(begin, begin + count,
                             [&](std::uint32_t a, std::uint32_t b) { return centroids_[a][axis] < centroids_[b][axis]; });
            mid = first + count / 2;
        }
        const auto left = static_cast<std::uint32_t>(nodes_.size());
        nodes_.push_back({});
        subdivide(left, first, mid - first, depth + 1);
        const auto right = static_cast<std::uint32_t>(nodes_.size());
        nodes_.push_back({});
        subdivide(right, mid, first + count - mid, depth + 1);
        nodes_[node].first = right;
        nodes_[node].count = 0;
        (void)left; // left == node + 1 by construction
    }

    std::vector<Tri> tris_;
    std::vector<BvhNode> nodes_;
    std::vector<std::uint32_t> order_;
    std::vector<Vec3> centroids_;
};

// --- Sampling ------------------------------------------------------------------------------

void basis(Vec3 n, Vec3& t, Vec3& b) {
    // Duff et al. 2017, "Building an Orthonormal Basis, Revisited".
    const float sign = std::copysign(1.0f, n.z);
    const float a = -1.0f / (sign + n.z);
    const float bb = n.x * n.y * a;
    t = {1.0f + sign * n.x * n.x * a, sign * bb, -sign * n.x};
    b = {bb, sign + n.y * n.y * a, -n.y};
}

Vec3 cosine_sample(Vec3 n, float u1, float u2) {
    const float r = std::sqrt(u1);
    const float phi = math::kTau * u2;
    Vec3 t, b;
    basis(n, t, b);
    const float z = std::sqrt(std::max(0.0f, 1.0f - u1));
    return math::normalize(t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + n * z);
}

float fract(float x) { return x - std::floor(x); }

// --- Scene shading -------------------------------------------------------------------------

struct InstanceShading {
    const LightmapInstance* src{nullptr};
    std::vector<Vec3> normals; // world-space vertex normals
};

class Tracer {
public:
    Tracer(const LightmapScene& scene, const LightmapSettings& settings) : scene_(scene), settings_(settings) {
        std::vector<Tri> tris;
        math::Aabb box;
        shading_.resize(scene.instances.size());
        for (std::uint32_t ii = 0; ii < scene.instances.size(); ++ii) {
            const LightmapInstance& inst = scene.instances[ii];
            shading_[ii].src = &inst;
            if (!inst.mesh) continue;
            const MeshData& m = *inst.mesh;
            const math::Mat4 nm = normal_matrix(inst.model);
            std::vector<Vec3> world(m.vertices.size());
            shading_[ii].normals.resize(m.vertices.size());
            for (std::size_t v = 0; v < m.vertices.size(); ++v) {
                world[v] = inst.model.transform_point(m.vertices[v].position);
                shading_[ii].normals[v] = math::normalize(nm.transform_vector(m.vertices[v].normal));
                box.expand(world[v]);
            }
            for (std::size_t i = 0; i + 2 < m.indices.size(); i += 3) {
                const std::uint32_t a = m.indices[i], b = m.indices[i + 1], c = m.indices[i + 2];
                if (a >= world.size() || b >= world.size() || c >= world.size()) continue;
                Tri t;
                t.v0 = world[a];
                t.e1 = world[b] - world[a];
                t.e2 = world[c] - world[a];
                const Vec3 cr = math::cross(t.e1, t.e2);
                const float len = math::length(cr);
                if (!(len > 1e-12f) || !finite(cr)) continue;
                t.n = cr / len;
                t.instance = ii;
                t.i0 = a;
                t.i1 = b;
                t.i2 = c;
                tris.push_back(t);
            }
        }
        triangles_ = tris.size();
        bvh_.build(std::move(tris));
        const float diag = box.valid() ? math::length(box.max - box.min) : 1.0f;
        eps_ = std::max(1e-4f, diag * 2e-5f);
    }

    [[nodiscard]] float eps() const { return eps_; }
    [[nodiscard]] std::size_t triangles() const { return triangles_; }
    [[nodiscard]] const Bvh& bvh() const { return bvh_; }

    Vec3 sky(Vec3 dir) const { return scene_.sky.evaluate(dir); }

    // Sum of light radiance * N.L arriving at p (shadowed), the shader's direct irradiance.
    // `dir_acc` (optional) gains each light's direction weighted by its luminance / pi.
    // `baked_only`: just the lights marked `baked` (their direct light belongs in the lightmap).
    Vec3 direct(Vec3 p, Vec3 n, std::uint64_t& rays, Vec3* dir_acc = nullptr, bool baked_only = false) const {
        Vec3 e{0, 0, 0};
        const Vec3 origin = p + n * eps_;
        for (const LightmapLight& l : scene_.lights)
            if (!baked_only || l.baked) for_each_light_sample(l, p, [&](Vec3 ldir, float dist, float atten, Vec3 radiance) {
                const float ndl = math::dot(n, ldir);
                if (ndl <= 0.0f || atten <= 0.0f) return;
                if (l.cast_shadows) {
                    ++rays;
                    const float tmax = l.type == LightmapLight::Type::Directional ? 1e30f : std::max(dist - 2.0f * eps_, 0.0f);
                    if (bvh_.occluded(make_ray(origin, ldir), tmax)) return;
                }
                e += radiance * (ndl * atten);
                if (dir_acc) *dir_acc += ldir * (luminance(radiance) * ndl * atten * (1.0f / math::kPi));
            });
        return e;
    }

    // Calls fn(direction to the light, distance, attenuation, radiance) for each sample of `l`
    // as seen from p: one for punctual lights, a grid of cell centres for area lights (each
    // with its share of the radiance and a Lambert emission factor).
    template <typename F> static void for_each_light_sample(const LightmapLight& l, Vec3 p, F&& fn) {
        if (l.type == LightmapLight::Type::Directional) {
            if (!(math::length(l.direction) > 1e-6f)) return;
            fn(-math::normalize(l.direction), std::numeric_limits<float>::max(), 1.0f, l.radiance);
            return;
        }
        if (l.type == LightmapLight::Type::Area) {
            const Vec3 nl = math::length(l.direction) > 1e-6f ? math::normalize(l.direction) : Vec3{0, -1, 0};
            Vec3 ex = l.right - nl * math::dot(l.right, nl);
            ex = math::length(ex) > 1e-6f ? math::normalize(ex) : math::normalize(math::cross(nl, std::abs(nl.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}));
            const Vec3 ey = math::cross(nl, ex);
            const float w = std::max(l.width, 0.01f), h = std::max(l.height, 0.01f);
            if (math::dot(p - l.position, nl) <= 0.0f) return; // behind the emitting face
            // Range is measured from the nearest point of the rectangle, as in the shader.
            const Vec3 d = p - l.position;
            const float qx = std::clamp(math::dot(d, ex), -w * 0.5f, w * 0.5f), qy = std::clamp(math::dot(d, ey), -h * 0.5f, h * 0.5f);
            const float near = math::length(l.position + ex * qx + ey * qy - p);
            if (near >= l.range) return;
            const float rr = near / std::max(l.range, 1e-4f);
            const float falloff = math::saturate(1.0f - rr * rr * rr * rr);
            const int nx = std::clamp(static_cast<int>(std::ceil(w / 0.125f)), 4, 8);
            const int ny = std::clamp(static_cast<int>(std::ceil(h / 0.125f)), 4, 8);
            const float cell = (w / static_cast<float>(nx)) * (h / static_cast<float>(ny));
            const Vec3 share = l.radiance * (1.0f / static_cast<float>(nx * ny));
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i) {
                    const Vec3 sp = l.position + ex * ((static_cast<float>(i) + 0.5f) / static_cast<float>(nx) - 0.5f) * w +
                                    ey * ((static_cast<float>(j) + 0.5f) / static_cast<float>(ny) - 0.5f) * h;
                    const Vec3 to = sp - p;
                    const float dist = math::length(to);
                    if (!(dist > 1e-5f)) continue;
                    const Vec3 ldir = to / dist;
                    const float emit = math::dot(-ldir, nl);
                    if (emit <= 0.0f) continue;
                    // A cell closer than its own size stops acting like a point: clamp its 1/d^2.
                    fn(ldir, dist, falloff * falloff * emit / std::max(dist * dist, cell), share);
                }
            return;
        }
        const Vec3 to = l.position - p;
        const float dist = math::length(to);
        if (!(dist > 1e-5f) || dist >= l.range) return;
        const Vec3 ldir = to / dist;
        const float r = dist / std::max(l.range, 1e-4f);
        const float falloff = math::saturate(1.0f - r * r * r * r);
        float atten = falloff * falloff / (dist * dist + 1.0f);
        Vec3 radiance = l.radiance;
        if (l.type == LightmapLight::Type::Spot) {
            const float cd = math::dot(-ldir, math::normalize(l.direction));
            const float t = math::saturate((cd - l.cos_outer) / std::max(l.cos_inner - l.cos_outer, 1e-4f));
            atten *= t * t;
            if (l.cookie && atten > 0.0f) radiance = radiance * spot_cookie_tint(l, -to);
        }
        fn(ldir, dist, atten, radiance);
    }

    struct Surface {
        Vec3 p, n, geo_n, albedo, emissive;
        bool backface{false};
    };

    Surface surface(const Ray& r, const Hit& h) const {
        const Tri& t = bvh_.tri(h.tri);
        const InstanceShading& sh = shading_[t.instance];
        const float w = 1.0f - h.u - h.v;
        Surface s;
        s.p = r.o + r.d * h.t;
        s.geo_n = t.n;
        s.backface = math::dot(r.d, t.n) > 0.0f;
        Vec3 n = sh.normals[t.i0] * w + sh.normals[t.i1] * h.u + sh.normals[t.i2] * h.v;
        n = math::normalize(n);
        if (!finite(n) || math::dot(n, n) < 0.5f) n = t.n;
        s.n = n;
        const LightmapInstance& inst = *sh.src;
        Vec3 albedo = inst.albedo;
        if (inst.albedo_texture && !inst.albedo_texture->empty() && inst.mesh) {
            const auto& vs = inst.mesh->vertices;
            const Vec2 uv = vs[t.i0].uv * w + vs[t.i1].uv * h.u + vs[t.i2].uv * h.v;
            const Image& img = *inst.albedo_texture;
            const float fx = fract(uv.x) * static_cast<float>(img.width);
            const float fy = fract(uv.y) * static_cast<float>(img.height);
            const auto x = std::min(static_cast<std::uint32_t>(fx), img.width - 1);
            const auto y = std::min(static_cast<std::uint32_t>(fy), img.height - 1);
            const std::uint32_t px = img.pixel(x, y);
            albedo = albedo * Vec3{srgb_to_linear(static_cast<float>(px & 0xff) / 255.0f),
                                   srgb_to_linear(static_cast<float>((px >> 8) & 0xff) / 255.0f),
                                   srgb_to_linear(static_cast<float>((px >> 16) & 0xff) / 255.0f)};
        }
        s.albedo = math::min(math::max(albedo, Vec3{0.0f}), Vec3{0.98f});
        s.emissive = inst.emissive;
        return s;
    }

    // Radiance arriving at `origin` along -`dir`: sky, emission and the bake lights' direct light
    // at every hit, for up to settings.bounces further diffuse bounces. `first_backface` is set
    // when the first hit is a back face (the origin is inside geometry); that path returns black.
    Vec3 trace_path(Vec3 origin, Vec3 dir, Random& rng, std::uint64_t& rays, bool& first_backface) const {
        Vec3 throughput{1, 1, 1};
        Vec3 radiance{0, 0, 0};
        for (std::uint32_t bounce = 0;; ++bounce) {
            ++rays;
            const Ray ray = make_ray(origin, dir);
            const Hit h = bvh_.closest(ray, 1e30f);
            if (!h.found) {
                radiance += throughput * sky(dir);
                break;
            }
            const Surface surf = surface(ray, h);
            if (surf.backface) {
                if (bounce == 0) first_backface = true;
                break;
            }
            radiance += throughput * surf.emissive;
            const Vec3 ed = direct(surf.p, surf.n, rays);
            radiance += throughput * surf.albedo * ed * (1.0f / math::kPi);
            if (bounce >= settings_.bounces) break;
            throughput = throughput * surf.albedo;
            if (std::max({throughput.x, throughput.y, throughput.z}) < 1e-3f) break;
            dir = cosine_sample(surf.n, rng.next_float(), rng.next_float());
            if (math::dot(dir, surf.geo_n) <= 0.0f) dir = math::normalize(dir - surf.geo_n * (2.0f * math::dot(dir, surf.geo_n)));
            origin = surf.p + surf.geo_n * eps_;
        }
        return radiance;
    }

    // Each bake light's unoccluded radiance at a free point (no surface normal), with the
    // direction it arrives from: what a light probe sees of the lights directly.
    template <typename F> void lights_at(Vec3 p, std::uint64_t& rays, F&& fn, bool baked_only = false) const {
        for (const LightmapLight& l : scene_.lights)
            if (!baked_only || l.baked) for_each_light_sample(l, p, [&](Vec3 ldir, float dist, float atten, Vec3 radiance) {
                if (atten <= 0.0f) return;
                if (l.cast_shadows) {
                    ++rays;
                    const float tmax = l.type == LightmapLight::Type::Directional ? 1e30f : std::max(dist - 2.0f * eps_, 0.0f);
                    if (bvh_.occluded(make_ray(p, ldir), tmax)) return;
                }
                fn(ldir, radiance * atten);
            });
    }

    // Irradiance / pi arriving at (p, n) from `samples` cosine-distributed paths. Returns the
    // fraction of first-bounce rays that hit back faces (texels inside geometry).
    // `std_error` is the standard error of the estimate's luminance (guides the denoiser).
    // `dir_sum` receives the first-ray directions weighted by each path's luminance / samples
    // (its length over the mean luminance is the directionality).
    Vec3 gather(Vec3 p, Vec3 n, Vec3 geo_n, std::uint64_t texel_id, float& backface_ratio, float& std_error,
                std::uint64_t& rays, Vec3& dir_sum) const {
        Random rng(settings_.seed ^ (texel_id * 0x9e3779b97f4a7c15ull), texel_id);
        const std::uint32_t samples = std::max(settings_.samples, 1u);
        const float jx = rng.next_float(), jy = rng.next_float();
        Vec3 sum{0, 0, 0};
        Vec3 dsum{0, 0, 0};
        double lum_sum = 0, lum_sq = 0;
        std::uint32_t backfaces = 0;
        for (std::uint32_t s = 0; s < samples; ++s) {
            // R2 low-discrepancy sequence (Roberts 2018), Cranley-Patterson rotated per texel.
            const float u1 = fract(jx + 0.7548776662f * static_cast<float>(s));
            const float u2 = fract(jy + 0.5698402910f * static_cast<float>(s));
            Vec3 dir = cosine_sample(n, u1, u2);
            if (math::dot(dir, geo_n) <= 0.0f) dir = math::normalize(dir - geo_n * (2.0f * math::dot(dir, geo_n)));
            const Vec3 first_dir = dir;
            bool backface = false;
            const Vec3 radiance = trace_path(p + geo_n * eps_, dir, rng, rays, backface);
            if (backface) ++backfaces;
            sum += radiance;
            const double l = 0.2126 * radiance.x + 0.7152 * radiance.y + 0.0722 * radiance.z;
            lum_sum += l;
            lum_sq += l * l;
            dsum += first_dir * static_cast<float>(l);
        }
        dir_sum = dsum / static_cast<float>(samples);
        const double mean = lum_sum / samples;
        const double var = std::max(0.0, lum_sq / samples - mean * mean);
        std_error = static_cast<float>(std::sqrt(var / samples));
        backface_ratio = static_cast<float>(backfaces) / static_cast<float>(samples);
        return sum / static_cast<float>(samples);
    }

private:
    const LightmapScene& scene_;
    const LightmapSettings& settings_;
    std::vector<InstanceShading> shading_;
    Bvh bvh_;
    std::size_t triangles_{0};
    float eps_{1e-4f};
};

// --- Texel rasterisation -------------------------------------------------------------------

struct Texel {
    Vec3 p, n, geo_n;
    float dist{std::numeric_limits<float>::max()}; // 0 = centre inside a triangle
    bool covered{false};
};

// Closest point of triangle (a, b, c) to q in 2D, as barycentrics (wa, wb, wc).
void closest_barycentric(Vec2 q, Vec2 a, Vec2 b, Vec2 c, float& wa, float& wb, float& wc, float& dist) {
    const Vec2 v0 = b - a, v1 = c - a, v2 = q - a;
    const float d00 = math::dot(v0, v0), d01 = math::dot(v0, v1), d11 = math::dot(v1, v1);
    const float d20 = math::dot(v2, v0), d21 = math::dot(v2, v1);
    const float den = d00 * d11 - d01 * d01;
    if (std::fabs(den) > 1e-12f) {
        const float v = (d11 * d20 - d01 * d21) / den;
        const float w = (d00 * d21 - d01 * d20) / den;
        const float u = 1.0f - v - w;
        if (u >= 0.0f && v >= 0.0f && w >= 0.0f) {
            wa = u;
            wb = v;
            wc = w;
            dist = 0.0f;
            return;
        }
    }
    // Outside: nearest point on the three edges.
    dist = std::numeric_limits<float>::max();
    auto edge = [&](Vec2 p0, Vec2 p1, int i0, int i1) {
        const Vec2 e = p1 - p0;
        const float len2 = math::dot(e, e);
        const float t = len2 > 0 ? math::saturate(math::dot(q - p0, e) / len2) : 0.0f;
        const Vec2 cp = p0 + e * t;
        const float d = math::length(q - cp);
        if (d < dist) {
            dist = d;
            float w3[3] = {0, 0, 0};
            w3[i0] = 1.0f - t;
            w3[i1] = t;
            wa = w3[0];
            wb = w3[1];
            wc = w3[2];
        }
    };
    edge(a, b, 0, 1);
    edge(b, c, 1, 2);
    edge(c, a, 2, 0);
}

// SH9 basis in the order and normalisation of pbr.frag's sh9() and SphericalHarmonics9.
std::array<float, 9> sh_basis(Vec3 n) noexcept {
    return {0.282095f,
            0.488603f * n.y,
            0.488603f * n.z,
            0.488603f * n.x,
            1.092548f * n.x * n.y,
            1.092548f * n.y * n.z,
            0.315392f * (3.0f * n.z * n.z - 1.0f),
            1.092548f * n.x * n.z,
            0.546274f * (n.x * n.x - n.y * n.y)};
}
// Cosine-lobe convolution A_l / pi per coefficient (radiance SH -> irradiance / pi).
constexpr float kShBand[9] = {1.0f, 2.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f};

Vec3 sh_eval(const std::array<Vec3, 9>& c, Vec3 n) noexcept {
    const auto b = sh_basis(n);
    Vec3 r{0, 0, 0};
    for (std::size_t i = 0; i < 9; ++i) r += c[i] * b[i];
    return math::max(r, Vec3{0.0f});
}

} // namespace

Vec3 LightmapSky::evaluate(Vec3 dir) const {
    if (radiance) return radiance(dir) * intensity;
    const float h = math::clamp(dir.y, -1.0f, 1.0f);
    const Vec3 c = h > 0.0f ? math::lerp(horizon, sky, std::sqrt(h)) : math::lerp(horizon, ground, std::pow(-h, 0.4f));
    return c * intensity;
}

namespace {

// World-space copy of one receiver (positions + normals) for xatlas.
struct UnwrapInput {
    std::uint32_t instance;
    std::vector<Vec3> positions, normals;
    std::vector<std::uint32_t> indices;
};

// Charts of `inputs` packed by xatlas into one atlas; uvs in texels of that atlas.
struct ChartAtlas {
    std::uint32_t width{0}, height{0};
    std::vector<LightmapUnwrap> unwraps; // per input, uv in texels
};

Result<ChartAtlas> xatlas_pack(const std::vector<const UnwrapInput*>& inputs, float tpu, std::uint32_t padding) {
    std::unique_ptr<xatlas::Atlas, void (*)(xatlas::Atlas*)> atlas(xatlas::Create(), xatlas::Destroy);
    for (const UnwrapInput* in : inputs) {
        xatlas::MeshDecl decl;
        decl.vertexPositionData = in->positions.data();
        decl.vertexPositionStride = sizeof(Vec3);
        decl.vertexNormalData = in->normals.data();
        decl.vertexNormalStride = sizeof(Vec3);
        decl.vertexCount = static_cast<std::uint32_t>(in->positions.size());
        decl.indexData = in->indices.data();
        decl.indexCount = static_cast<std::uint32_t>(in->indices.size());
        decl.indexFormat = xatlas::IndexFormat::UInt32;
        const xatlas::AddMeshError err = xatlas::AddMesh(atlas.get(), decl, static_cast<std::uint32_t>(inputs.size()));
        if (err != xatlas::AddMeshError::Success)
            return make_error(ErrorCode::InvalidArgument, std::string("lightmap unwrap failed: ") + xatlas::StringForEnum(err));
    }
    xatlas::ChartOptions chart;
    xatlas::PackOptions pack;
    pack.texelsPerUnit = tpu;
    pack.padding = padding;
    pack.bilinear = true;
    pack.blockAlign = true;
    pack.resolution = 0; // one atlas sized to fit
    xatlas::Generate(atlas.get(), chart, pack);
    ChartAtlas out;
    out.width = atlas->width;
    out.height = atlas->height;
    if (out.width == 0 || out.height == 0 || atlas->meshCount != inputs.size())
        return make_error(ErrorCode::InvalidArgument, "lightmap unwrap produced an empty atlas");
    out.unwraps.resize(inputs.size());
    for (std::uint32_t mi = 0; mi < atlas->meshCount; ++mi) {
        const xatlas::Mesh& om = atlas->meshes[mi];
        LightmapUnwrap& u = out.unwraps[mi];
        u.xref.resize(om.vertexCount);
        u.uv.resize(om.vertexCount);
        for (std::uint32_t v = 0; v < om.vertexCount; ++v) {
            u.xref[v] = om.vertexArray[v].xref;
            u.uv[v] = {om.vertexArray[v].uv[0], om.vertexArray[v].uv[1]};
        }
        u.indices.assign(om.indexArray, om.indexArray + om.indexCount);
    }
    return out;
}

// Receivers whose meshes are identical and whose model matrices differ only by a rotation,
// reflection-free, plus translation (equal Gram matrix M^T M of the linear part) are congruent:
// one unwrap is valid for all of them at the same texel density (ADR-0135).
struct UvGroup {
    std::uint64_t mesh{0};
    std::size_t vertices{0}, indices{0};
    std::array<float, 6> gram{};
    bool mirrored{false};
    std::vector<std::uint32_t> members; // indices into the inputs
};

std::array<float, 6> gram_of(const math::Mat4& m, bool& mirrored) {
    const Vec3 c0{m.c[0].x, m.c[0].y, m.c[0].z}, c1{m.c[1].x, m.c[1].y, m.c[1].z}, c2{m.c[2].x, m.c[2].y, m.c[2].z};
    mirrored = math::dot(math::cross(c0, c1), c2) < 0.0f;
    return {math::dot(c0, c0), math::dot(c1, c1), math::dot(c2, c2), math::dot(c0, c1), math::dot(c0, c2), math::dot(c1, c2)};
}

bool same_gram(const std::array<float, 6>& a, const std::array<float, 6>& b) {
    float scale = 1e-6f;
    for (int i = 0; i < 3; ++i) scale = std::max({scale, std::abs(a[i]), std::abs(b[i])});
    for (int i = 0; i < 6; ++i)
        if (std::abs(a[i] - b[i]) > 1e-5f * scale) return false;
    return true;
}

// Shelf packing of rectangles (texels, gap already included); deterministic. Returns the atlas
// size and each rectangle's corner, trying a few widths and keeping the smallest square side.
struct ShelfResult {
    std::uint32_t width{0}, height{0};
    std::vector<std::array<std::uint32_t, 2>> at;
};
ShelfResult shelf_pack(const std::vector<std::array<std::uint32_t, 2>>& rects) {
    std::vector<std::uint32_t> order(rects.size());
    for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        if (rects[a][1] != rects[b][1]) return rects[a][1] > rects[b][1];
        return rects[a][0] > rects[b][0];
    });
    std::uint64_t area = 0;
    std::uint32_t widest = 0;
    for (const auto& r : rects) {
        area += std::uint64_t{r[0]} * r[1];
        widest = std::max(widest, r[0]);
    }
    ShelfResult best;
    std::uint64_t best_side = std::numeric_limits<std::uint64_t>::max(), best_area = best_side;
    const double root = std::sqrt(static_cast<double>(area));
    for (const double f : {0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 2.0}) {
        const std::uint32_t limit = std::max(widest, static_cast<std::uint32_t>(std::ceil(root * f)));
        ShelfResult r;
        r.at.resize(rects.size());
        std::uint32_t x = 0, y = 0, shelf = 0;
        for (const std::uint32_t i : order) {
            if (x > 0 && x + rects[i][0] > limit) {
                y += shelf;
                x = shelf = 0;
            }
            r.at[i] = {x, y};
            x += rects[i][0];
            shelf = std::max(shelf, rects[i][1]);
            r.width = std::max(r.width, x);
        }
        r.height = y + shelf;
        const std::uint64_t side = std::max(r.width, r.height), a = std::uint64_t{r.width} * r.height;
        if (side < best_side || (side == best_side && a < best_area)) {
            best_side = side;
            best_area = a;
            best = std::move(r);
        }
    }
    return best;
}

} // namespace

Result<LightmapLayout> unwrap_lightmaps(const LightmapScene& scene, const LightmapSettings& settings) {
    VICTUS_PROFILE_SCOPE("unwrap_lightmaps");
    if (!(settings.texels_per_unit > 0.0f) || !std::isfinite(settings.texels_per_unit))
        return make_error(ErrorCode::InvalidArgument, "lightmap texels_per_unit must be positive");
    if (settings.max_atlas_size < 16 || settings.max_atlas_size > 8192)
        return make_error(ErrorCode::InvalidArgument, "lightmap max_atlas_size must be 16..8192");
    std::vector<UnwrapInput> inputs;
    for (std::uint32_t ii = 0; ii < scene.instances.size(); ++ii) {
        const LightmapInstance& inst = scene.instances[ii];
        if (!inst.receives || !inst.mesh || inst.mesh->indices.size() < 3) continue;
        const MeshData& m = *inst.mesh;
        UnwrapInput in;
        in.instance = ii;
        const math::Mat4 nm = normal_matrix(inst.model);
        in.positions.reserve(m.vertices.size());
        in.normals.reserve(m.vertices.size());
        for (const Vertex3D& v : m.vertices) {
            in.positions.push_back(inst.model.transform_point(v.position));
            in.normals.push_back(math::normalize(nm.transform_vector(v.normal)));
        }
        for (std::size_t i = 0; i + 2 < m.indices.size(); i += 3) {
            const std::uint32_t a = m.indices[i], b = m.indices[i + 1], c = m.indices[i + 2];
            if (a >= m.vertices.size() || b >= m.vertices.size() || c >= m.vertices.size())
                return make_error(ErrorCode::InvalidArgument, "lightmap mesh index out of range");
            in.indices.insert(in.indices.end(), {a, b, c});
        }
        inputs.push_back(std::move(in));
    }
    LightmapLayout layout;
    layout.instances.resize(scene.instances.size());
    if (inputs.empty()) return layout;

    // Congruent receivers share one unwrap (ADR-0135), so their uvs differ by a translation only
    // and the renderer can draw them from one mesh, instanced, with a per-instance uv offset.
    std::vector<UvGroup> groups;
    if (settings.share_uvs) {
        std::unordered_map<const MeshData*, std::uint64_t> prints;
        for (std::uint32_t k = 0; k < inputs.size(); ++k) {
            const LightmapInstance& inst = scene.instances[inputs[k].instance];
            auto [pit, fresh] = prints.try_emplace(inst.mesh, 0);
            if (fresh) pit->second = lightmap_mesh_fingerprint(*inst.mesh);
            bool mirrored = false;
            const auto gram = gram_of(inst.model, mirrored);
            UvGroup* g = nullptr;
            for (UvGroup& c : groups)
                if (c.mesh == pit->second && c.vertices == inst.mesh->vertices.size() && c.indices == inst.mesh->indices.size() &&
                    c.mirrored == mirrored && same_gram(c.gram, gram)) {
                    g = &c;
                    break;
                }
            if (!g) {
                groups.push_back({pit->second, inst.mesh->vertices.size(), inst.mesh->indices.size(), gram, mirrored, {}});
                g = &groups.back();
            }
            g->members.push_back(k);
        }
    }
    const bool shared = std::any_of(groups.begin(), groups.end(), [](const UvGroup& g) { return g.members.size() > 1; });

    float tpu = settings.texels_per_unit;
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (!shared) {
            // Every receiver unique: one xatlas run packs all charts together (tightest atlas).
            std::vector<const UnwrapInput*> all;
            for (const UnwrapInput& in : inputs) all.push_back(&in);
            auto packed = xatlas_pack(all, tpu, settings.padding);
            if (!packed) return std::move(packed).error();
            const std::uint32_t w = packed->width, h = packed->height;
            if (std::max(w, h) > settings.max_atlas_size) {
                // Area scales with density squared; aim a little under the limit.
                tpu *= 0.95f * static_cast<float>(settings.max_atlas_size) / static_cast<float>(std::max(w, h));
                continue;
            }
            layout.width = w;
            layout.height = h;
            layout.texels_per_unit = tpu;
            for (std::uint32_t k = 0; k < inputs.size(); ++k) {
                LightmapUnwrap& u = layout.instances[inputs[k].instance];
                u = std::move(packed->unwraps[k]);
                for (Vec2& uv : u.uv) uv = {uv.x / static_cast<float>(w), uv.y / static_cast<float>(h)};
            }
            return layout;
        }
        // Blocks: every shared group's representative unwrapped on its own (one copy per member),
        // and all unique receivers packed together by xatlas into one more block; then the blocks
        // are shelf-packed into the atlas.
        std::vector<ChartAtlas> atlases;
        std::vector<std::vector<std::uint32_t>> atlas_inputs;
        std::vector<const UnwrapInput*> singles;
        std::vector<std::uint32_t> single_ids;
        for (const UvGroup& g : groups) {
            if (g.members.size() == 1) {
                singles.push_back(&inputs[g.members[0]]);
                single_ids.push_back(g.members[0]);
                continue;
            }
            auto packed = xatlas_pack({&inputs[g.members[0]]}, tpu, settings.padding);
            if (!packed) return std::move(packed).error();
            atlases.push_back(std::move(*packed));
            atlas_inputs.push_back(g.members);
        }
        if (!singles.empty()) {
            auto packed = xatlas_pack(singles, tpu, settings.padding);
            if (!packed) return std::move(packed).error();
            atlases.push_back(std::move(*packed));
            atlas_inputs.push_back({}); // marker: one block holding every single
        }
        // One rectangle per shared member, one for the singles block.
        struct Rect {
            std::uint32_t atlas, member; // member: index into atlas_inputs[atlas] (shared) or ~0u (singles)
        };
        std::vector<Rect> rects;
        std::vector<std::array<std::uint32_t, 2>> sizes;
        const std::uint32_t gap = std::max(settings.padding, 1u);
        for (std::uint32_t a = 0; a < atlases.size(); ++a) {
            const std::array<std::uint32_t, 2> size{atlases[a].width + gap, atlases[a].height + gap};
            if (atlas_inputs[a].empty()) {
                rects.push_back({a, ~0u});
                sizes.push_back(size);
            } else {
                for (std::uint32_t m = 0; m < atlas_inputs[a].size(); ++m) {
                    rects.push_back({a, m});
                    sizes.push_back(size);
                }
            }
        }
        const ShelfResult shelf = shelf_pack(sizes);
        const std::uint32_t w = shelf.width, h = shelf.height;
        if (w == 0 || h == 0) return make_error(ErrorCode::InvalidArgument, "lightmap unwrap produced an empty atlas");
        if (std::max(w, h) > settings.max_atlas_size) {
            tpu *= 0.95f * static_cast<float>(settings.max_atlas_size) / static_cast<float>(std::max(w, h));
            continue;
        }
        layout.width = w;
        layout.height = h;
        layout.texels_per_unit = tpu;
        const auto place = [&](const LightmapUnwrap& src, std::array<std::uint32_t, 2> at, LightmapUnwrap& dst) {
            dst.xref = src.xref;
            dst.indices = src.indices;
            dst.uv.resize(src.uv.size());
            for (std::size_t v = 0; v < src.uv.size(); ++v)
                dst.uv[v] = {(src.uv[v].x + static_cast<float>(at[0])) / static_cast<float>(w),
                             (src.uv[v].y + static_cast<float>(at[1])) / static_cast<float>(h)};
        };
        for (std::size_t r = 0; r < rects.size(); ++r) {
            const ChartAtlas& ca = atlases[rects[r].atlas];
            if (rects[r].member == ~0u) {
                for (std::size_t k = 0; k < single_ids.size(); ++k)
                    place(ca.unwraps[k], shelf.at[r], layout.instances[inputs[single_ids[k]].instance]);
            } else {
                const std::uint32_t input = atlas_inputs[rects[r].atlas][rects[r].member];
                place(ca.unwraps[0], shelf.at[r], layout.instances[inputs[input].instance]);
            }
        }
        return layout;
    }
    return make_error(ErrorCode::LimitExceeded, "lightmap scene does not fit the atlas; lower texels_per_unit");
}

float LightmapProbes::sample_sun(Vec3 p) const noexcept {
    if (sun.size() != sh.size() || sh.empty()) return 1.0f;
    const Vec3 size = bounds.max - bounds.min;
    Vec3 f{};
    for (int a = 0; a < 3; ++a) {
        if (!(p[a] >= bounds.min[a] && p[a] <= bounds.max[a])) return 1.0f;
        const auto n = static_cast<float>(counts[static_cast<std::size_t>(a)] - 1);
        f[a] = std::clamp((p[a] - bounds.min[a]) / std::max(size[a], 1e-6f) * n, 0.0f, n);
    }
    const std::uint32_t x0 = std::min(static_cast<std::uint32_t>(f.x), counts[0] - 2);
    const std::uint32_t y0 = std::min(static_cast<std::uint32_t>(f.y), counts[1] - 2);
    const std::uint32_t z0 = std::min(static_cast<std::uint32_t>(f.z), counts[2] - 2);
    const Vec3 t{f.x - static_cast<float>(x0), f.y - static_cast<float>(y0), f.z - static_cast<float>(z0)};
    float v = 0.0f, weight = 0.0f;
    for (std::uint32_t c = 0; c < 8; ++c) {
        const std::uint32_t dx = c & 1u, dy = (c >> 1) & 1u, dz = (c >> 2) & 1u;
        const float w = (dx ? t.x : 1 - t.x) * (dy ? t.y : 1 - t.y) * (dz ? t.z : 1 - t.z);
        const std::uint32_t i = (((y0 + dy) * counts[2]) + z0 + dz) * counts[0] + x0 + dx;
        if (!valid[i] || w <= 0.0f) continue;
        v += sun[i] * w;
        weight += w;
    }
    if (weight <= 1e-5f) return 1.0f;
    return 1.0f + (v / weight - 1.0f) * std::min(weight, 1.0f);
}

std::uint32_t LightmapProbes::valid_count() const noexcept {
    return static_cast<std::uint32_t>(std::count_if(valid.begin(), valid.end(), [](std::uint8_t v) { return v != 0; }));
}

Vec3 LightmapProbes::sample(Vec3 p, Vec3 n, Vec3 fallback) const noexcept {
    if (empty() || sh.size() != probe_count() || valid.size() != sh.size()) return fallback;
    Vec3 f{};
    for (int a = 0; a < 3; ++a) {
        const auto ua = static_cast<std::size_t>(a);
        if (p[a] < bounds.min[a] || p[a] > bounds.max[a]) return fallback;
        const float extent = bounds.max[a] - bounds.min[a];
        const float cells = static_cast<float>(counts[ua] - 1);
        f[a] = extent > 0.0f ? std::clamp((p[a] - bounds.min[a]) / extent * cells, 0.0f, cells) : 0.0f;
    }
    const std::uint32_t x0 = std::min(static_cast<std::uint32_t>(f.x), counts[0] - 2);
    const std::uint32_t y0 = std::min(static_cast<std::uint32_t>(f.y), counts[1] - 2);
    const std::uint32_t z0 = std::min(static_cast<std::uint32_t>(f.z), counts[2] - 2);
    const Vec3 t{f.x - static_cast<float>(x0), f.y - static_cast<float>(y0), f.z - static_cast<float>(z0)};
    std::array<Vec3, 9> sum{};
    float weight = 0.0f;
    for (std::uint32_t c = 0; c < 8; ++c) {
        const std::uint32_t dx = c & 1u, dy = (c >> 1) & 1u, dz = (c >> 2) & 1u;
        const float w = (dx ? t.x : 1 - t.x) * (dy ? t.y : 1 - t.y) * (dz ? t.z : 1 - t.z);
        const std::uint32_t i = ((y0 + dy) * counts[2] + (z0 + dz)) * counts[0] + (x0 + dx);
        if (!valid[i] || w <= 0.0f) continue;
        for (std::size_t k = 0; k < 9; ++k) sum[k] += sh[i][k] * w;
        weight += w;
    }
    if (weight <= 1e-5f) return fallback;
    for (auto& c : sum) c = c * (1.0f / weight);
    const Vec3 v = sh_eval(sum, n);
    return fallback + (v - fallback) * std::min(weight, 1.0f);
}

namespace {

// Probe grid for a scene: probe_bounds when valid, else every instance's world bounds padded by
// a quarter metre, and at least 3 m tall so characters standing on a flat floor are inside.
Result<IrradianceGrid> probe_grid(const LightmapScene& scene, const LightmapSettings& settings) {
    math::Aabb box = settings.probe_bounds;
    const bool fixed = box.valid() && finite(box.min) && finite(box.max);
    if (!fixed) {
        box = {};
        for (const LightmapInstance& inst : scene.instances) {
            if (!inst.mesh) continue;
            for (const Vertex3D& v : inst.mesh->vertices) {
                const Vec3 w = inst.model.transform_point(v.position);
                if (finite(w)) box.expand(w);
            }
        }
        if (!box.valid()) return make_error(ErrorCode::InvalidArgument, "light probes: the scene has no geometry");
        box.min -= Vec3{0.25f};
        box.max += Vec3{0.25f};
        box.max.y = std::max(box.max.y, box.min.y + 3.0f);
    }
    IrradianceGrid grid;
    grid.bounds = box;
    grid.counts = irradiance_counts_for(box.max - box.min, settings.probe_spacing);
    // Twice the vertical density: dynamic objects (characters) mostly move horizontally, and
    // with only two layers in a 3 m tall room both end up outside the interesting band (one in
    // the floor, one above a table or roof) - the shadowing under cover is lost.
    grid.counts[1] = irradiance_counts_for(box.max - box.min, settings.probe_spacing * 0.5f)[1];
    return grid.sanitized();
}

// SH9 irradiance / pi at a free point: `samples` uniformly distributed paths (spherical
// Fibonacci, rotated per probe) plus, in fully baked mode, the bake lights' direct light.
// `inside` is set when more than a quarter of the first hits are back faces.
// The light the sun shadowmask (ADR-0131) and probe sun occlusion (ADR-0136) are baked for: the
// first shadow-casting directional light, when settings.shadowmask is on.
const LightmapLight* shadowmask_light(const LightmapScene& scene, const LightmapSettings& settings) {
    if (!settings.shadowmask) return nullptr;
    for (const LightmapLight& l : scene.lights)
        if (l.type == LightmapLight::Type::Directional && l.cast_shadows && math::length(l.direction) > 1e-6f) return &l;
    return nullptr;
}

// Fraction of `samples` rays from `origin` towards the sun's disc (uniform over it, an R2
// sequence offset by `jitter`) that leave the scene.
struct SunDisc {
    Vec3 l{0, 1, 0}, t{1, 0, 0}, b{0, 0, 1};
    float spread{0};
    std::uint32_t samples{1};
};
SunDisc sun_disc(const LightmapLight& light, const LightmapSettings& settings) {
    SunDisc d;
    d.l = -math::normalize(light.direction);
    basis(d.l, d.t, d.b);
    d.spread = std::tan(std::clamp(settings.sun_angular_radius, 0.0f, 0.2f));
    d.samples = std::max(settings.shadowmask_samples, 1u);
    return d;
}
float disc_visibility(const Tracer& tracer, const SunDisc& disc, Vec3 origin, float jitter, std::uint64_t& rays) {
    std::uint32_t lit = 0;
    for (std::uint32_t s = 0; s < disc.samples; ++s) {
        // Concentric-free disc sampling: R2 sequence mapped to a disc (sqrt radius).
        const float u1 = fract(jitter + 0.7548776662f * static_cast<float>(s));
        const float u2 = fract(jitter + 0.5698402910f * static_cast<float>(s));
        const float r = std::sqrt(u1) * disc.spread, phi = 2.0f * math::kPi * u2;
        const Vec3 d = math::normalize(disc.l + disc.t * (r * std::cos(phi)) + disc.b * (r * std::sin(phi)));
        ++rays;
        if (!tracer.bvh().occluded(make_ray(origin, d), 1e30f)) ++lit;
    }
    return static_cast<float>(lit) / static_cast<float>(disc.samples);
}

std::array<Vec3, 9> bake_probe(const Tracer& tracer, Vec3 p, std::uint32_t samples, bool include_direct, std::uint64_t probe_id,
                               std::uint64_t seed, bool& inside, std::uint64_t& rays) {
    Random rng(seed ^ 0x9b0be5u ^ (probe_id * 0xd1b54a32d192ed03ull), probe_id + 1);
    std::array<Vec3, 9> L{};
    const float jz = rng.next_float(), jphi = rng.next_float();
    std::uint32_t backfaces = 0;
    for (std::uint32_t s = 0; s < samples; ++s) {
        const float z = 1.0f - 2.0f * fract((static_cast<float>(s) + 0.5f) / static_cast<float>(samples) + jz);
        const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        const float phi = 2.0f * math::kPi * fract(static_cast<float>(s) * 0.6180339887f + jphi);
        const Vec3 dir{r * std::cos(phi), z, r * std::sin(phi)}; // y up: uniform in cos(theta)
        bool backface = false;
        const Vec3 radiance = tracer.trace_path(p, dir, rng, rays, backface);
        if (backface) {
            ++backfaces;
            continue;
        }
        const auto b = sh_basis(dir);
        for (std::size_t k = 0; k < 9; ++k) L[k] += radiance * b[k];
    }
    const float w = 4.0f * math::kPi / static_cast<float>(samples);
    for (auto& c : L) c = c * w;
    tracer.lights_at(
        p, rays,
        [&](Vec3 ldir, Vec3 radiance) {
            const auto b = sh_basis(ldir);
            for (std::size_t k = 0; k < 9; ++k) L[k] += radiance * b[k];
        },
        !include_direct);
    for (std::size_t k = 0; k < 9; ++k) L[k] = L[k] * kShBand[k];
    inside = static_cast<float>(backfaces) > 0.25f * static_cast<float>(samples);
    return L;
}

Result<LightmapProbes> bake_probes_with(const Tracer& tracer, const LightmapScene& scene, const LightmapSettings& settings,
                                        jobs::JobSystem* jobs, LightmapBakeStats* stats) {
    VICTUS_PROFILE_SCOPE("bake_light_probes");
    const auto t0 = std::chrono::steady_clock::now();
    auto grid = probe_grid(scene, settings);
    if (!grid) return std::move(grid).error();
    LightmapProbes out;
    out.bounds = grid->bounds;
    out.counts = grid->counts;
    const std::uint32_t count = grid->probe_count();
    out.sh.assign(count, {});
    out.valid.assign(count, 0);
    const std::uint32_t samples = std::clamp(settings.probe_samples, 16u, 8192u);
    std::vector<std::uint64_t> rays(count, 0);
    // Probe sun occlusion (ADR-0136, Unity's probe occlusion): the shadowmask light's visibility
    // at each probe, so dynamic objects keep baked sun shadows past the realtime shadow distance.
    const LightmapLight* mask = shadowmask_light(scene, settings);
    const SunDisc disc = mask ? sun_disc(*mask, settings) : SunDisc{};
    if (mask) out.sun.assign(count, 1.0f);
    auto run = [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i) {
            const auto idx = static_cast<std::uint32_t>(i);
            bool inside = false;
            const Vec3 at = grid->probe_position(idx);
            out.sh[i] = bake_probe(tracer, at, samples, settings.include_direct, i, settings.seed, inside, rays[i]);
            out.valid[i] = inside ? 0 : 1;
            if (mask && !inside) out.sun[i] = disc_visibility(tracer, disc, at, fract(static_cast<float>(i) * 0.618034f), rays[i]);
            if (inside) out.sh[i] = {};
            for (auto& c : out.sh[i])
                if (!finite(c)) c = {0, 0, 0};
        }
    };
    if (jobs) jobs->parallel_for(0, count, 8, run);
    else run(0, count);
    const std::uint32_t invalid = count - out.valid_count();
    std::uint32_t dilated = 0;
    if (settings.probe_dilation && invalid > 0) {
        // Dilation (Unity APV's "Probe Dilation"): an invalid probe takes the inverse-squared-
        // distance average of its valid neighbours in the surrounding 3x3x3 block, so objects
        // next to (or overlapping) geometry interpolate full-weight neighbour lighting instead of
        // fading to the sky. Only raw bake results are sources: one pass, order independent.
        const std::vector<std::uint8_t> raw = out.valid;
        const auto& c = out.counts;
        for (std::uint32_t y = 0; y < c[1]; ++y)
            for (std::uint32_t z = 0; z < c[2]; ++z)
                for (std::uint32_t x = 0; x < c[0]; ++x) {
                    const std::uint32_t i = (y * c[2] + z) * c[0] + x;
                    if (raw[i]) continue;
                    std::array<Vec3, 9> sum{};
                    float weight = 0.0f, sun = 0.0f;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dz = -1; dz <= 1; ++dz)
                            for (int dx = -1; dx <= 1; ++dx) {
                                const auto nx = static_cast<std::int64_t>(x) + dx, ny = static_cast<std::int64_t>(y) + dy,
                                           nz = static_cast<std::int64_t>(z) + dz;
                                if (nx < 0 || ny < 0 || nz < 0 || nx >= c[0] || ny >= c[1] || nz >= c[2]) continue;
                                const auto j = static_cast<std::size_t>((ny * c[2] + nz) * c[0] + nx);
                                if (!raw[j]) continue;
                                const float w = 1.0f / static_cast<float>(dx * dx + dy * dy + dz * dz);
                                for (std::size_t k = 0; k < 9; ++k) sum[k] += out.sh[j][k] * w;
                                if (!out.sun.empty()) sun += out.sun[j] * w;
                                weight += w;
                            }
                    if (weight <= 0.0f) continue;
                    for (std::size_t k = 0; k < 9; ++k) out.sh[i][k] = sum[k] * (1.0f / weight);
                    if (!out.sun.empty()) out.sun[i] = sun / weight;
                    out.valid[i] = 1;
                    ++dilated;
                }
    }
    if (stats) {
        stats->probes = count;
        stats->invalid_probes = invalid;
        stats->dilated_probes = dilated;
        for (std::uint64_t r : rays) stats->rays += r;
        stats->probe_ms = ms_since(t0);
    }
    return out;
}

} // namespace

Result<LightmapProbes> bake_light_probes(const LightmapScene& scene, const LightmapSettings& settings, jobs::JobSystem* jobs,
                                         LightmapBakeStats* stats) {
    const Tracer tracer(scene, settings);
    return bake_probes_with(tracer, scene, settings, jobs, stats);
}

Result<LightmapBake> bake_lightmaps(const LightmapScene& scene, const LightmapSettings& settings, jobs::JobSystem* jobs) {
    VICTUS_PROFILE_SCOPE("bake_lightmaps");
    LightmapBake bake;
    auto t0 = std::chrono::steady_clock::now();
    auto layout = unwrap_lightmaps(scene, settings);
    if (!layout) return std::move(layout).error();
    bake.stats.unwrap_ms = ms_since(t0);
    bake.instances = std::move(layout.value().instances);
    const std::uint32_t W = layout.value().width, H = layout.value().height;
    bake.stats.width = W;
    bake.stats.height = H;
    bake.stats.texels_per_unit = layout.value().texels_per_unit;
    if (W == 0 || H == 0) return bake;

    // Rasterise receivers into texels (world position and normals at texel centres). Texels whose
    // centre is inside a triangle win; partially covered edge texels take the nearest point.
    std::vector<Texel> texels(static_cast<std::size_t>(W) * H);
    for (std::uint32_t ii = 0; ii < bake.instances.size(); ++ii) {
        const LightmapUnwrap& u = bake.instances[ii];
        if (u.empty()) continue;
        const LightmapInstance& inst = scene.instances[ii];
        const MeshData& m = *inst.mesh;
        const math::Mat4 nm = normal_matrix(inst.model);
        for (std::size_t i = 0; i + 2 < u.indices.size(); i += 3) {
            const std::uint32_t k[3] = {u.indices[i], u.indices[i + 1], u.indices[i + 2]};
            Vec2 uv[3];
            Vec3 p[3], n[3];
            for (int j = 0; j < 3; ++j) {
                uv[j] = {u.uv[k[j]].x * static_cast<float>(W), u.uv[k[j]].y * static_cast<float>(H)};
                const Vertex3D& v = m.vertices[u.xref[k[j]]];
                p[j] = inst.model.transform_point(v.position);
                n[j] = math::normalize(nm.transform_vector(v.normal));
            }
            Vec3 gn = math::cross(p[1] - p[0], p[2] - p[0]);
            if (!(math::length(gn) > 1e-12f)) continue;
            gn = math::normalize(gn);
            const int x0 = std::max(0, static_cast<int>(std::floor(std::min({uv[0].x, uv[1].x, uv[2].x}) - 1.0f)));
            const int y0 = std::max(0, static_cast<int>(std::floor(std::min({uv[0].y, uv[1].y, uv[2].y}) - 1.0f)));
            const int x1 = std::min(static_cast<int>(W) - 1, static_cast<int>(std::ceil(std::max({uv[0].x, uv[1].x, uv[2].x}) + 1.0f)));
            const int y1 = std::min(static_cast<int>(H) - 1, static_cast<int>(std::ceil(std::max({uv[0].y, uv[1].y, uv[2].y}) + 1.0f)));
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    float wa = 0, wb = 0, wc = 0, d = 0;
                    closest_barycentric({static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f}, uv[0], uv[1], uv[2], wa, wb,
                                        wc, d);
                    if (d > 0.75f) continue;
                    Texel& t = texels[static_cast<std::size_t>(y) * W + static_cast<std::size_t>(x)];
                    if (t.covered && t.dist <= d) continue;
                    t.covered = true;
                    t.dist = d;
                    t.p = p[0] * wa + p[1] * wb + p[2] * wc;
                    Vec3 nn = math::normalize(n[0] * wa + n[1] * wb + n[2] * wc);
                    if (!finite(nn) || math::dot(nn, nn) < 0.5f) nn = gn;
                    t.n = nn;
                    t.geo_n = gn;
                }
            }
        }
    }

    // Trace.
    t0 = std::chrono::steady_clock::now();
    const Tracer tracer(scene, settings);
    bake.stats.triangles = tracer.triangles();
    std::vector<Vec4> result(texels.size(), Vec4{0, 0, 0, 0});
    std::vector<float> noise(texels.size(), 0.0f); // standard error of each texel's luminance
    // Luminance-weighted incoming direction per texel (directional bakes), normalised later.
    std::vector<Vec3> dirs(settings.directional ? texels.size() : 0, Vec3{0, 0, 0});
    // Shadowmask (ADR-0131): visibility of the first shadow-casting directional light.
    const LightmapLight* mask_light = shadowmask_light(scene, settings);
    std::vector<float> vis(mask_light ? texels.size() : 0, 1.0f);
    const SunDisc disc = mask_light ? sun_disc(*mask_light, settings) : SunDisc{};
    auto sun_visibility = [&](const Texel& t, std::size_t texel_id, std::uint64_t& rays) {
        if (math::dot(t.geo_n, disc.l) <= 0.0f) return 0.0f; // faces away: self-shadowed
        return disc_visibility(tracer, disc, t.p + t.geo_n * tracer.eps(), fract(static_cast<float>(texel_id) * 0.618034f), rays);
    };
    std::vector<std::uint64_t> row_rays(H, 0);
    auto trace_rows = [&](std::size_t rb, std::size_t re) {
        for (std::size_t y = rb; y < re; ++y) {
            std::uint64_t rays = 0;
            for (std::size_t x = 0; x < W; ++x) {
                const std::size_t i = y * W + x;
                const Texel& t = texels[i];
                if (!t.covered) continue;
                float backface = 0;
                Vec3 dsum{0, 0, 0};
                Vec3 e = tracer.gather(t.p, t.n, t.geo_n, i, backface, noise[i], rays, dsum);
                e += tracer.direct(t.p, t.n, rays, &dsum, !settings.include_direct) * (1.0f / math::kPi);
                if (!vis.empty()) vis[i] = sun_visibility(t, i, rays);
                if (!dirs.empty()) {
                    const float le = luminance(e);
                    dirs[i] = le > 1e-8f ? dsum / le : Vec3{0, 0, 0};
                }
                // Texels that mostly see back faces are inside geometry (behind walls, under
                // floors): drop them so dilation fills them from valid neighbours (no leaks).
                result[i] = backface > 0.25f ? Vec4{0, 0, 0, -1} : Vec4{e.x, e.y, e.z, 1};
            }
            row_rays[y] = rays;
        }
    };
    if (jobs) {
        jobs->parallel_for(0, H, 1, trace_rows);
    } else {
        trace_rows(0, H);
    }
    for (std::uint64_t r : row_rays) bake.stats.rays += r;
    bake.stats.trace_ms = ms_since(t0);
    if (settings.probes) {
        // Same tracer, sky and bounce settings as the texels, so dynamic objects match the
        // scenery they stand next to (ADR-0133).
        auto probes = bake_probes_with(tracer, scene, settings, jobs, &bake.stats);
        if (!probes) return std::move(probes).error();
        bake.probes = std::move(*probes);
    }

    // Post: count, denoise, dilate.
    t0 = std::chrono::steady_clock::now();
    for (const Vec4& v : result) {
        if (v.w > 0.5f) ++bake.stats.texels;
        if (v.w < 0.0f) ++bake.stats.invalid_texels;
    }
    for (Vec4& v : result)
        if (v.w < 0.0f) v.w = 0.0f;

    if (settings.denoise) {
        // Edge-aware a-trous wavelet filter (Dammertz et al. 2010) over covered texels, guided by
        // world position and normal so neighbouring charts of other surfaces never blend, and by
        // each texel's measured noise (as in SVGF, Schied et al. 2017) so only differences the
        // noise can explain are smoothed: real gradients such as contact shadows survive.
        const float texel_world = 1.0f / std::max(bake.stats.texels_per_unit, 1e-3f);
        const float kernel[3] = {3.0f / 8.0f, 1.0f / 4.0f, 1.0f / 16.0f};
        std::vector<Vec4> tmp(result.size());
        std::vector<float> tmp_noise(result.size());
        std::vector<Vec3> tmp_dirs(dirs.size());
        auto lum = [](const Vec4& v) { return 0.2126f * v.x + 0.7152f * v.y + 0.0722f * v.z; };
        for (int pass = 0; pass < 3; ++pass) {
            const int step = 1 << pass;
            const float sigma_p = texel_world * static_cast<float>(step) * 2.0f;
            for (std::uint32_t y = 0; y < H; ++y) {
                for (std::uint32_t x = 0; x < W; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * W + x;
                    if (result[i].w <= 0.5f) {
                        tmp[i] = result[i];
                        tmp_noise[i] = noise[i];
                        if (!dirs.empty()) tmp_dirs[i] = dirs[i];
                        continue;
                    }
                    const Texel& c = texels[i];
                    const float lc = lum(result[i]);
                    Vec3 acc{0, 0, 0}, dacc{0, 0, 0};
                    float wsum = 0, var_acc = 0;
                    for (int dy = -2; dy <= 2; ++dy) {
                        const int yy = static_cast<int>(y) + dy * step;
                        if (yy < 0 || yy >= static_cast<int>(H)) continue;
                        for (int dx = -2; dx <= 2; ++dx) {
                            const int xx = static_cast<int>(x) + dx * step;
                            if (xx < 0 || xx >= static_cast<int>(W)) continue;
                            const std::size_t j = static_cast<std::size_t>(yy) * W + static_cast<std::size_t>(xx);
                            if (result[j].w <= 0.5f) continue;
                            const Texel& q = texels[j];
                            const float nd = std::max(0.0f, math::dot(c.n, q.n));
                            const float wn = std::pow(nd, 32.0f);
                            const Vec3 dp = q.p - c.p;
                            const float wp = std::exp(-math::dot(dp, dp) / (sigma_p * sigma_p));
                            const float sigma_l = 3.0f * std::sqrt(noise[i] * noise[i] + noise[j] * noise[j]) + 1e-4f;
                            const float wl = std::exp(-std::fabs(lum(result[j]) - lc) / sigma_l);
                            const float w = kernel[std::abs(dx)] * kernel[std::abs(dy)] * wn * wp * wl;
                            acc += Vec3{result[j].x, result[j].y, result[j].z} * w;
                            if (!dirs.empty()) dacc += dirs[j] * (w * lum(result[j])); // keep it luminance-weighted
                            wsum += w;
                            var_acc += w * w * noise[j] * noise[j];
                        }
                    }
                    const Vec3 v = wsum > 0 ? acc / wsum : Vec3{result[i].x, result[i].y, result[i].z};
                    tmp[i] = {v.x, v.y, v.z, 1.0f};
                    if (!dirs.empty()) {
                        const float lv = wsum > 0 ? luminance(v) * wsum : 0.0f;
                        tmp_dirs[i] = lv > 1e-8f ? dacc / lv : dirs[i];
                    }
                    // Noise left after this pass (weighted mean of independent estimates).
                    tmp_noise[i] = wsum > 0 ? std::sqrt(var_acc) / wsum : noise[i];
                }
            }
            result.swap(tmp);
            noise.swap(tmp_noise);
            if (!dirs.empty()) dirs.swap(tmp_dirs);
        }
    }

    // Directional texels: (direction, rebalancing term at the baked normal). Dilation then grows
    // them together with the irradiance.
    std::vector<Vec4> dir_texels(dirs.size(), Vec4{0, 0, 0, 0});
    for (std::size_t i = 0; i < dirs.size(); ++i) {
        if (result[i].w <= 0.5f) continue;
        Vec3 d = dirs[i];
        const float len = math::length(d);
        if (!(len <= 1.0f)) d = len > 0.0f && std::isfinite(len) ? d / len : Vec3{0, 0, 0};
        const float rebalance = std::max(0.5f * math::dot(texels[i].n, d) + 0.5f, 0.5f);
        dir_texels[i] = {d.x, d.y, d.z, rebalance};
    }
    // Dilation: grow covered texels into the padding so bilinear filtering and mips never pull
    // in black from outside a chart.
    std::vector<Vec4> grown = result;
    std::vector<Vec4> grown_dirs = dir_texels;
    std::vector<float> grown_vis = vis;
    for (std::uint32_t it = 0; it < settings.dilation; ++it) {
        bool changed = false;
        for (std::uint32_t y = 0; y < H; ++y) {
            for (std::uint32_t x = 0; x < W; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * W + x;
                if (result[i].w > 0.0f) continue;
                Vec3 acc{0, 0, 0};
                Vec4 dacc{0, 0, 0, 0};
                float vacc = 0;
                float n = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = static_cast<int>(x) + dx, yy = static_cast<int>(y) + dy;
                        if (xx < 0 || yy < 0 || xx >= static_cast<int>(W) || yy >= static_cast<int>(H)) continue;
                        const Vec4& q = result[static_cast<std::size_t>(yy) * W + static_cast<std::size_t>(xx)];
                        if (q.w <= 0.0f) continue;
                        acc += Vec3{q.x, q.y, q.z};
                        const std::size_t j = static_cast<std::size_t>(yy) * W + static_cast<std::size_t>(xx);
                        if (!dir_texels.empty()) dacc = dacc + dir_texels[j];
                        if (!vis.empty()) vacc += vis[j];
                        n += 1.0f;
                    }
                }
                if (n > 0) {
                    const Vec3 v = acc / n;
                    // Dilated texels keep a small positive coverage so later passes grow from them
                    // but they are distinguishable from real ones.
                    grown[i] = {v.x, v.y, v.z, 0.25f};
                    if (!dir_texels.empty()) grown_dirs[i] = dacc * (1.0f / n);
                    if (!vis.empty()) grown_vis[i] = vacc / n;
                    changed = true;
                }
            }
        }
        result = grown;
        dir_texels = grown_dirs;
        vis = grown_vis;
        if (!changed) break;
    }

    bake.atlas.width = W;
    bake.atlas.height = H;
    bake.atlas.pixels.resize(result.size() * 4);
    for (std::size_t i = 0; i < result.size(); ++i) {
        bake.atlas.pixels[i * 4 + 0] = result[i].x;
        bake.atlas.pixels[i * 4 + 1] = result[i].y;
        bake.atlas.pixels[i * 4 + 2] = result[i].z;
        bake.atlas.pixels[i * 4 + 3] = !vis.empty() ? (result[i].w > 0.0f ? vis[i] : 1.0f)
                                                    : (result[i].w >= 1.0f ? 1.0f : (result[i].w > 0.0f ? 0.5f : 0.0f));
    }
    if (mask_light) bake.shadowmask_direction = math::normalize(mask_light->direction);
    if (!dir_texels.empty()) {
        bake.direction.width = W;
        bake.direction.height = H;
        bake.direction.pixels.resize(dir_texels.size() * 4);
        auto unorm = [](float v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
        for (std::size_t i = 0; i < dir_texels.size(); ++i) {
            const Vec4& d = dir_texels[i];
            const bool has = d.w >= 0.25f;
            bake.direction.pixels[i * 4 + 0] = unorm(has ? d.x * 0.5f + 0.5f : 0.5f);
            bake.direction.pixels[i * 4 + 1] = unorm(has ? d.y * 0.5f + 0.5f : 0.5f);
            bake.direction.pixels[i * 4 + 2] = unorm(has ? d.z * 0.5f + 0.5f : 0.5f);
            bake.direction.pixels[i * 4 + 3] = unorm(has ? d.w : 0.0f);
        }
    }
    bake.stats.post_ms = ms_since(t0);
    return bake;
}

Vec3 spot_cookie_tint(const LightmapLight& light, Vec3 to_surface) noexcept {
    const Image* img = light.cookie.get();
    if (!img || img->width == 0 || img->height == 0 || img->pixels.size() < std::size_t{img->width} * img->height * 4)
        return {1, 1, 1};
    const Vec3 dir = math::length(light.direction) > 1e-6f ? math::normalize(light.direction) : Vec3{0, -1, 0};
    Vec3 right = light.right - dir * math::dot(light.right, dir);
    if (math::length(right) < 1e-4f) right = std::abs(dir.x) < 0.9f ? math::cross(Vec3{1, 0, 0}, dir) : math::cross(Vec3{0, 1, 0}, dir);
    right = math::normalize(right);
    const Vec3 up = math::cross(right, dir);
    const float z = math::dot(to_surface, dir);
    if (z <= 1e-5f) return {0, 0, 0};
    const float c = std::clamp(light.cos_outer, 0.0215f, 1.0f); // outer angle below ~88.8 degrees, as the renderer
    const float tan_outer = std::sqrt(std::max(1.0f - c * c, 0.0f)) / c;
    const float u = std::clamp(0.5f + 0.5f * math::dot(to_surface, right) / (z * tan_outer), 0.0f, 1.0f);
    const float v = std::clamp(0.5f - 0.5f * math::dot(to_surface, up) / (z * tan_outer), 0.0f, 1.0f);
    const float fx = std::clamp(u * static_cast<float>(img->width) - 0.5f, 0.0f, static_cast<float>(img->width - 1));
    const float fy = std::clamp(v * static_cast<float>(img->height) - 0.5f, 0.0f, static_cast<float>(img->height - 1));
    const auto x0 = static_cast<std::uint32_t>(fx), y0 = static_cast<std::uint32_t>(fy);
    const std::uint32_t x1 = std::min(x0 + 1, img->width - 1), y1 = std::min(y0 + 1, img->height - 1);
    const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
    const auto at = [&](std::uint32_t x, std::uint32_t y) {
        const std::uint8_t* p = &img->pixels[(std::size_t{y} * img->width + x) * 4];
        return Vec3{math::srgb_to_linear(p[0] / 255.0f), math::srgb_to_linear(p[1] / 255.0f), math::srgb_to_linear(p[2] / 255.0f)};
    };
    const Vec3 top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
    const Vec3 bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
    return top + (bottom - top) * ty;
}

float lightmap_directional_scale(math::Vec4 texel, Vec3 normal) noexcept {
    if (!(texel.w >= 0.25f)) return 1.0f;
    const Vec3 d{texel.x * 2.0f - 1.0f, texel.y * 2.0f - 1.0f, texel.z * 2.0f - 1.0f};
    return (0.5f * math::dot(normal, d) + 0.5f) / texel.w;
}

math::Vec4 sample_lightmap(const ImageF& atlas, Vec2 uv) noexcept {
    if (atlas.empty() || atlas.width == 0 || atlas.height == 0) return {0, 0, 0, 0};
    const float fx = math::clamp(uv.x, 0.0f, 1.0f) * static_cast<float>(atlas.width) - 0.5f;
    const float fy = math::clamp(uv.y, 0.0f, 1.0f) * static_cast<float>(atlas.height) - 0.5f;
    const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
    auto at = [&](int x, int y) {
        x = std::clamp(x, 0, static_cast<int>(atlas.width) - 1);
        y = std::clamp(y, 0, static_cast<int>(atlas.height) - 1);
        return atlas.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
    };
    const Vec4 a = math::lerp(at(x0, y0), at(x0 + 1, y0), tx);
    const Vec4 b = math::lerp(at(x0, y0 + 1), at(x0 + 1, y0 + 1), tx);
    return math::lerp(a, b, ty);
}

MeshData apply_lightmap_unwrap(const MeshData& mesh, const LightmapUnwrap& unwrap) {
    MeshData out;
    out.vertices.reserve(unwrap.xref.size());
    for (std::uint32_t x : unwrap.xref) out.vertices.push_back(x < mesh.vertices.size() ? mesh.vertices[x] : Vertex3D{});
    out.indices = unwrap.indices;
    return out;
}

std::optional<Vec2> lightmap_uv_offset(const LightmapUnwrap& base, const LightmapUnwrap& unwrap, float tolerance) noexcept {
    if (base.empty() || base.uv.size() != unwrap.uv.size() || base.xref != unwrap.xref || base.indices != unwrap.indices)
        return std::nullopt;
    const Vec2 off = unwrap.uv[0] - base.uv[0];
    for (std::size_t i = 0; i < base.uv.size(); ++i) {
        const Vec2 d = unwrap.uv[i] - base.uv[i] - off;
        if (!(std::abs(d.x) <= tolerance && std::abs(d.y) <= tolerance)) return std::nullopt;
    }
    return off;
}

std::uint64_t lightmap_mesh_fingerprint(const MeshData& mesh) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&](const void* data, std::size_t size) {
        h = fnv1a64(std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data), size), h);
    };
    const std::uint64_t counts[2] = {mesh.vertices.size(), mesh.indices.size()};
    mix(counts, sizeof(counts));
    for (const Vertex3D& v : mesh.vertices) {
        mix(&v.position, sizeof(v.position));
        mix(&v.normal, sizeof(v.normal));
    }
    if (!mesh.indices.empty()) mix(mesh.indices.data(), mesh.indices.size() * sizeof(std::uint32_t));
    return h;
}

// --- .vlightmap ----------------------------------------------------------------------------

namespace {

constexpr char kMagic[8] = {'V', 'I', 'C', 'T', 'U', 'S', 'L', 'M'};
constexpr std::uint32_t kMaxAtlasSide = 8192;
constexpr std::uint32_t kFlagDirectional = 1u; // version 2 header flag: an RGBA8 directional atlas follows the atlas
constexpr std::uint32_t kFlagShadowmask = 2u;  // version 2 header flag: 3 floats (shadowmask light direction) follow the flags
constexpr std::uint32_t kFlagProbes = 4u;      // version 3 header flag: a light probe block follows the directional atlas
constexpr std::uint32_t kFlagProbeSun = 8u;    // version 4 header flag: one sun visibility byte per probe follows the probes
constexpr std::size_t kProbeBytes = 1 + 27 * 2; // validity byte + 27 half floats

bool probes_encodable(const LightmapProbes& p) {
    const std::uint32_t n = p.counts[0] * p.counts[1] * p.counts[2];
    return !p.sh.empty() && p.sh.size() == n && p.valid.size() == n && p.counts[0] >= 2 && p.counts[1] >= 2 && p.counts[2] >= 2 &&
           p.counts[0] <= kMaxIrradianceProbesPerAxis && p.counts[1] <= kMaxIrradianceProbesY && p.counts[2] <= kMaxIrradianceProbesPerAxis &&
           p.bounds.valid() && finite(p.bounds.min) && finite(p.bounds.max);
}

class Writer {
public:
    void bytes(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    }
    void u16(std::uint16_t v) {
        out.push_back(static_cast<std::uint8_t>(v));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
    }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void f32(float f) {
        std::uint32_t v = 0;
        std::memcpy(&v, &f, 4);
        u32(v);
    }
    std::vector<std::uint8_t> out;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> b) : b_(b) {}
    [[nodiscard]] bool has(std::size_t n) const { return n <= b_.size() - pos_; }
    bool u16(std::uint16_t& v) {
        if (!has(2)) return false;
        v = static_cast<std::uint16_t>(b_[pos_] | (b_[pos_ + 1] << 8));
        pos_ += 2;
        return true;
    }
    bool u32(std::uint32_t& v) {
        if (!has(4)) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(b_[pos_ + i]) << (8 * i);
        pos_ += 4;
        return true;
    }
    bool u64(std::uint64_t& v) {
        if (!has(8)) return false;
        v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(b_[pos_ + i]) << (8 * i);
        pos_ += 8;
        return true;
    }
    bool f32(float& f) {
        std::uint32_t v = 0;
        if (!u32(v)) return false;
        std::memcpy(&f, &v, 4);
        return true;
    }
    bool bytes(void* p, std::size_t n) {
        if (!has(n)) return false;
        std::memcpy(p, b_.data() + pos_, n);
        pos_ += n;
        return true;
    }
    [[nodiscard]] std::size_t remaining() const { return b_.size() - pos_; }

private:
    std::span<const std::uint8_t> b_;
    std::size_t pos_{0};
};

} // namespace

const LightmapFileEntry* LightmapFile::find(const Uuid& entity) const noexcept {
    for (const LightmapFileEntry& e : entries)
        if (e.entity == entity) return &e;
    return nullptr;
}

std::vector<std::uint8_t> encode_lightmap_file(const LightmapFile& file) {
    Writer w;
    w.bytes(kMagic, sizeof(kMagic));
    w.u32(kLightmapFileVersion);
    w.u32(file.atlas.width);
    w.u32(file.atlas.height);
    w.u32(static_cast<std::uint32_t>(file.entries.size()));
    const bool directional = file.direction.width == file.atlas.width && file.direction.height == file.atlas.height &&
                             file.direction.pixels.size() == static_cast<std::size_t>(file.atlas.width) * file.atlas.height * 4 &&
                             !file.direction.pixels.empty();
    const bool shadowmask = math::length(file.shadowmask_direction) > 0.5f && finite(file.shadowmask_direction);
    const bool probes = probes_encodable(file.probes);
    const bool probe_sun = probes && file.probes.sun.size() == file.probes.sh.size();
    w.u32((directional ? kFlagDirectional : 0u) | (shadowmask ? kFlagShadowmask : 0u) | (probes ? kFlagProbes : 0u) |
          (probe_sun ? kFlagProbeSun : 0u));
    if (shadowmask) {
        const Vec3 d = math::normalize(file.shadowmask_direction);
        w.f32(d.x);
        w.f32(d.y);
        w.f32(d.z);
    }
    const std::size_t texels = static_cast<std::size_t>(file.atlas.width) * file.atlas.height;
    for (std::size_t i = 0; i < texels * 4; ++i) w.u16(float_to_half(i < file.atlas.pixels.size() ? file.atlas.pixels[i] : 0.0f));
    if (directional) w.bytes(file.direction.pixels.data(), file.direction.pixels.size());
    if (probes) {
        const LightmapProbes& p = file.probes;
        for (int a = 0; a < 3; ++a) w.f32(p.bounds.min[a]);
        for (int a = 0; a < 3; ++a) w.f32(p.bounds.max[a]);
        for (std::uint32_t c : p.counts) w.u32(c);
        for (std::size_t i = 0; i < p.sh.size(); ++i) {
            w.out.push_back(p.valid[i] ? 1 : 0);
            for (const Vec3& c : p.sh[i])
                for (int k = 0; k < 3; ++k) w.u16(float_to_half(c[k]));
        }
        if (probe_sun)
            for (const float v : p.sun)
                w.out.push_back(static_cast<std::uint8_t>(std::lround(std::clamp(std::isfinite(v) ? v : 1.0f, 0.0f, 1.0f) * 255.0f)));
    }
    for (const LightmapFileEntry& e : file.entries) {
        w.u64(e.entity.hi);
        w.u64(e.entity.lo);
        w.u64(e.mesh_fingerprint);
        w.u32(static_cast<std::uint32_t>(e.unwrap.xref.size()));
        w.u32(static_cast<std::uint32_t>(e.unwrap.indices.size()));
        for (std::uint32_t x : e.unwrap.xref) w.u32(x);
        for (std::size_t i = 0; i < e.unwrap.xref.size(); ++i) {
            const Vec2 uv = i < e.unwrap.uv.size() ? e.unwrap.uv[i] : Vec2{};
            w.f32(uv.x);
            w.f32(uv.y);
        }
        for (std::uint32_t idx : e.unwrap.indices) w.u32(idx);
    }
    return std::move(w.out);
}

Result<LightmapFile> decode_lightmap_file(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    char magic[8];
    if (!r.bytes(magic, 8) || std::memcmp(magic, kMagic, 8) != 0)
        return make_error(ErrorCode::ParseError, "not a VICTUS lightmap file");
    std::uint32_t version = 0, w = 0, h = 0, count = 0;
    if (!r.u32(version) || !r.u32(w) || !r.u32(h) || !r.u32(count)) return make_error(ErrorCode::ParseError, "truncated lightmap header");
    if (version < 1 || version > kLightmapFileVersion)
        return make_error(ErrorCode::VersionMismatch, "unsupported lightmap file version " + std::to_string(version));
    std::uint32_t flags = 0;
    if (version >= 2 && !r.u32(flags)) return make_error(ErrorCode::ParseError, "truncated lightmap header");
    const std::uint32_t known = kFlagDirectional | kFlagShadowmask | (version >= 3 ? kFlagProbes : 0u) | (version >= 4 ? kFlagProbeSun : 0u);
    if ((flags & ~known) != 0) return make_error(ErrorCode::ParseError, "unknown lightmap file flags");
    if ((flags & kFlagProbeSun) != 0 && (flags & kFlagProbes) == 0)
        return make_error(ErrorCode::ParseError, "lightmap probe sun occlusion without probes");
    Vec3 mask_dir{0, 0, 0};
    if ((flags & kFlagShadowmask) != 0) {
        if (!r.f32(mask_dir.x) || !r.f32(mask_dir.y) || !r.f32(mask_dir.z))
            return make_error(ErrorCode::ParseError, "truncated lightmap shadowmask header");
        const float len = math::length(mask_dir);
        if (!finite(mask_dir) || !(len > 0.5f && len < 1.5f)) return make_error(ErrorCode::ParseError, "invalid lightmap shadowmask direction");
        mask_dir = mask_dir / len;
    }
    if (w > kMaxAtlasSide || h > kMaxAtlasSide || (w == 0) != (h == 0))
        return make_error(ErrorCode::ParseError, "invalid lightmap atlas size");
    LightmapFile file;
    file.shadowmask_direction = mask_dir;
    const std::size_t values = static_cast<std::size_t>(w) * h * 4;
    if (!r.has(values * 2)) return make_error(ErrorCode::ParseError, "truncated lightmap atlas");
    file.atlas.width = w;
    file.atlas.height = h;
    file.atlas.pixels.resize(values);
    for (std::size_t i = 0; i < values; ++i) {
        std::uint16_t hv = 0;
        (void)r.u16(hv);
        const float f = half_to_float(hv);
        file.atlas.pixels[i] = std::isfinite(f) ? f : 0.0f;
    }
    if ((flags & kFlagDirectional) != 0) {
        if (w == 0 || !r.has(values)) return make_error(ErrorCode::ParseError, "truncated directional lightmap atlas");
        file.direction.width = w;
        file.direction.height = h;
        file.direction.pixels.resize(values);
        (void)r.bytes(file.direction.pixels.data(), values);
    }
    if ((flags & kFlagProbes) != 0) {
        LightmapProbes& p = file.probes;
        if (!r.f32(p.bounds.min.x) || !r.f32(p.bounds.min.y) || !r.f32(p.bounds.min.z) || !r.f32(p.bounds.max.x) ||
            !r.f32(p.bounds.max.y) || !r.f32(p.bounds.max.z) || !r.u32(p.counts[0]) || !r.u32(p.counts[1]) || !r.u32(p.counts[2]))
            return make_error(ErrorCode::ParseError, "truncated lightmap probe header");
        if (!finite(p.bounds.min) || !finite(p.bounds.max) || !p.bounds.valid())
            return make_error(ErrorCode::ParseError, "invalid lightmap probe bounds");
        if (p.counts[0] < 2 || p.counts[1] < 2 || p.counts[2] < 2 || p.counts[0] > kMaxIrradianceProbesPerAxis ||
            p.counts[1] > kMaxIrradianceProbesY || p.counts[2] > kMaxIrradianceProbesPerAxis)
            return make_error(ErrorCode::ParseError, "invalid lightmap probe counts");
        const std::size_t n = p.probe_count();
        if (!r.has(n * kProbeBytes)) return make_error(ErrorCode::ParseError, "truncated lightmap probes");
        p.sh.resize(n);
        p.valid.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            std::uint8_t v = 0;
            (void)r.bytes(&v, 1);
            if (v > 1) return make_error(ErrorCode::ParseError, "invalid lightmap probe validity");
            p.valid[i] = v;
            for (Vec3& c : p.sh[i])
                for (int k = 0; k < 3; ++k) {
                    std::uint16_t hv = 0;
                    (void)r.u16(hv);
                    const float f = half_to_float(hv);
                    c[k] = std::isfinite(f) ? f : 0.0f;
                }
        }
        if ((flags & kFlagProbeSun) != 0) {
            if (!r.has(n)) return make_error(ErrorCode::ParseError, "truncated lightmap probe sun occlusion");
            p.sun.resize(n);
            for (float& v : p.sun) {
                std::uint8_t b = 0;
                (void)r.bytes(&b, 1);
                v = static_cast<float>(b) / 255.0f;
            }
        }
    }
    // Each entry needs at least 32 bytes, which bounds `count` by the remaining size.
    if (count > r.remaining() / 32) return make_error(ErrorCode::ParseError, "invalid lightmap entry count");
    file.entries.resize(count);
    for (LightmapFileEntry& e : file.entries) {
        std::uint32_t vc = 0, ic = 0;
        if (!r.u64(e.entity.hi) || !r.u64(e.entity.lo) || !r.u64(e.mesh_fingerprint) || !r.u32(vc) || !r.u32(ic))
            return make_error(ErrorCode::ParseError, "truncated lightmap entry");
        if (ic % 3 != 0) return make_error(ErrorCode::ParseError, "lightmap entry index count is not a multiple of 3");
        const std::size_t need = static_cast<std::size_t>(vc) * 12 + static_cast<std::size_t>(ic) * 4;
        if (!r.has(need)) return make_error(ErrorCode::ParseError, "truncated lightmap entry data");
        e.unwrap.xref.resize(vc);
        e.unwrap.uv.resize(vc);
        e.unwrap.indices.resize(ic);
        for (std::uint32_t& x : e.unwrap.xref) (void)r.u32(x);
        for (Vec2& uv : e.unwrap.uv) {
            (void)r.f32(uv.x);
            (void)r.f32(uv.y);
            if (!std::isfinite(uv.x) || !std::isfinite(uv.y)) return make_error(ErrorCode::ParseError, "non-finite lightmap uv");
        }
        for (std::uint32_t& idx : e.unwrap.indices) {
            (void)r.u32(idx);
            if (idx >= vc) return make_error(ErrorCode::ParseError, "lightmap entry index out of range");
        }
    }
    return file;
}

} // namespace victus::render
