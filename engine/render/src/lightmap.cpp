// SPDX-License-Identifier: Apache-2.0
// Baked lighting (ADR-0122): xatlas unwrap, texel rasterisation, a SAH BVH and a CPU path tracer
// that bakes irradiance / pi into a lightmap atlas, then dilation and an edge-aware denoise.
#include "victus/render/lightmap.hpp"

#include "victus/core/jobs.hpp"
#include "victus/core/profiler.hpp"

#include <xatlas.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

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
    Vec3 direct(Vec3 p, Vec3 n, std::uint64_t& rays, Vec3* dir_acc = nullptr) const {
        Vec3 e{0, 0, 0};
        const Vec3 origin = p + n * eps_;
        for (const LightmapLight& l : scene_.lights) {
            Vec3 ldir;
            float dist = std::numeric_limits<float>::max();
            float atten = 1.0f;
            if (l.type == LightmapLight::Type::Directional) {
                ldir = -math::normalize(l.direction);
            } else {
                const Vec3 to = l.position - p;
                dist = math::length(to);
                if (!(dist > 1e-5f) || dist >= l.range) continue;
                ldir = to / dist;
                const float r = dist / std::max(l.range, 1e-4f);
                const float falloff = math::saturate(1.0f - r * r * r * r);
                atten = falloff * falloff / (dist * dist + 1.0f);
                if (l.type == LightmapLight::Type::Spot) {
                    const float cd = math::dot(-ldir, math::normalize(l.direction));
                    const float t = math::saturate((cd - l.cos_outer) / std::max(l.cos_inner - l.cos_outer, 1e-4f));
                    atten *= t * t;
                }
            }
            const float ndl = math::dot(n, ldir);
            if (ndl <= 0.0f || atten <= 0.0f) continue;
            if (l.cast_shadows) {
                ++rays;
                const float tmax = l.type == LightmapLight::Type::Directional ? 1e30f : std::max(dist - 2.0f * eps_, 0.0f);
                if (bvh_.occluded(make_ray(origin, ldir), tmax)) continue;
            }
            e += l.radiance * (ndl * atten);
            if (dir_acc) *dir_acc += ldir * (luminance(l.radiance) * ndl * atten * (1.0f / math::kPi));
        }
        return e;
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
            Vec3 origin = p + geo_n * eps_;
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
                    if (bounce == 0) ++backfaces;
                    break;
                }
                radiance += throughput * surf.emissive;
                const Vec3 ed = direct(surf.p, surf.n, rays);
                radiance += throughput * surf.albedo * ed * (1.0f / math::kPi);
                if (bounce >= settings_.bounces) break;
                throughput = throughput * surf.albedo;
                if (std::max({throughput.x, throughput.y, throughput.z}) < 1e-3f) break;
                dir = cosine_sample(surf.n, rng.next_float(), rng.next_float());
                if (math::dot(dir, surf.geo_n) <= 0.0f)
                    dir = math::normalize(dir - surf.geo_n * (2.0f * math::dot(dir, surf.geo_n)));
                origin = surf.p + surf.geo_n * eps_;
            }
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

} // namespace

Vec3 LightmapSky::evaluate(Vec3 dir) const {
    if (radiance) return radiance(dir) * intensity;
    const float h = math::clamp(dir.y, -1.0f, 1.0f);
    const Vec3 c = h > 0.0f ? math::lerp(horizon, sky, std::sqrt(h)) : math::lerp(horizon, ground, std::pow(-h, 0.4f));
    return c * intensity;
}

Result<LightmapLayout> unwrap_lightmaps(const LightmapScene& scene, const LightmapSettings& settings) {
    VICTUS_PROFILE_SCOPE("unwrap_lightmaps");
    if (!(settings.texels_per_unit > 0.0f) || !std::isfinite(settings.texels_per_unit))
        return make_error(ErrorCode::InvalidArgument, "lightmap texels_per_unit must be positive");
    if (settings.max_atlas_size < 16 || settings.max_atlas_size > 8192)
        return make_error(ErrorCode::InvalidArgument, "lightmap max_atlas_size must be 16..8192");
    // World-space copies of every receiver (positions + normals) for xatlas.
    struct Input {
        std::uint32_t instance;
        std::vector<Vec3> positions, normals;
        std::vector<std::uint32_t> indices;
    };
    std::vector<Input> inputs;
    for (std::uint32_t ii = 0; ii < scene.instances.size(); ++ii) {
        const LightmapInstance& inst = scene.instances[ii];
        if (!inst.receives || !inst.mesh || inst.mesh->indices.size() < 3) continue;
        const MeshData& m = *inst.mesh;
        Input in;
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

    float tpu = settings.texels_per_unit;
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::unique_ptr<xatlas::Atlas, void (*)(xatlas::Atlas*)> atlas(xatlas::Create(), xatlas::Destroy);
        for (const Input& in : inputs) {
            xatlas::MeshDecl decl;
            decl.vertexPositionData = in.positions.data();
            decl.vertexPositionStride = sizeof(Vec3);
            decl.vertexNormalData = in.normals.data();
            decl.vertexNormalStride = sizeof(Vec3);
            decl.vertexCount = static_cast<std::uint32_t>(in.positions.size());
            decl.indexData = in.indices.data();
            decl.indexCount = static_cast<std::uint32_t>(in.indices.size());
            decl.indexFormat = xatlas::IndexFormat::UInt32;
            const xatlas::AddMeshError err = xatlas::AddMesh(atlas.get(), decl, static_cast<std::uint32_t>(inputs.size()));
            if (err != xatlas::AddMeshError::Success)
                return make_error(ErrorCode::InvalidArgument, std::string("lightmap unwrap failed: ") + xatlas::StringForEnum(err));
        }
        xatlas::ChartOptions chart;
        xatlas::PackOptions pack;
        pack.texelsPerUnit = tpu;
        pack.padding = settings.padding;
        pack.bilinear = true;
        pack.blockAlign = true;
        pack.resolution = 0; // one atlas sized to fit
        xatlas::Generate(atlas.get(), chart, pack);
        const std::uint32_t w = atlas->width, h = atlas->height;
        if (w == 0 || h == 0) return make_error(ErrorCode::InvalidArgument, "lightmap unwrap produced an empty atlas");
        const std::uint32_t side = std::max(w, h);
        if (side > settings.max_atlas_size) {
            // Area scales with density squared; aim a little under the limit.
            tpu *= 0.95f * static_cast<float>(settings.max_atlas_size) / static_cast<float>(side);
            continue;
        }
        layout.width = w;
        layout.height = h;
        layout.texels_per_unit = tpu;
        for (std::uint32_t mi = 0; mi < atlas->meshCount; ++mi) {
            const xatlas::Mesh& om = atlas->meshes[mi];
            LightmapUnwrap& u = layout.instances[inputs[mi].instance];
            u.xref.resize(om.vertexCount);
            u.uv.resize(om.vertexCount);
            for (std::uint32_t v = 0; v < om.vertexCount; ++v) {
                u.xref[v] = om.vertexArray[v].xref;
                u.uv[v] = {om.vertexArray[v].uv[0] / static_cast<float>(w), om.vertexArray[v].uv[1] / static_cast<float>(h)};
            }
            u.indices.assign(om.indexArray, om.indexArray + om.indexCount);
        }
        return layout;
    }
    return make_error(ErrorCode::LimitExceeded, "lightmap scene does not fit the atlas; lower texels_per_unit");
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
    const LightmapLight* mask_light = nullptr;
    if (settings.shadowmask)
        for (const LightmapLight& l : scene.lights)
            if (l.type == LightmapLight::Type::Directional && l.cast_shadows && math::length(l.direction) > 1e-6f) {
                mask_light = &l;
                break;
            }
    std::vector<float> vis(mask_light ? texels.size() : 0, 1.0f);
    Vec3 mask_l{0, 1, 0}, mask_t{1, 0, 0}, mask_b{0, 0, 1};
    if (mask_light) {
        mask_l = -math::normalize(mask_light->direction);
        basis(mask_l, mask_t, mask_b);
    }
    const float mask_spread = std::tan(std::clamp(settings.sun_angular_radius, 0.0f, 0.2f));
    const std::uint32_t mask_samples = std::max(settings.shadowmask_samples, 1u);
    // Fraction of rays towards the sun's disc (uniform over it) that leave the scene.
    auto sun_visibility = [&](const Texel& t, std::size_t texel_id, std::uint64_t& rays) {
        if (math::dot(t.geo_n, mask_l) <= 0.0f) return 0.0f; // faces away: self-shadowed
        const Vec3 origin = t.p + t.geo_n * tracer.eps();
        const float j = fract(static_cast<float>(texel_id) * 0.618034f);
        std::uint32_t lit = 0;
        for (std::uint32_t s = 0; s < mask_samples; ++s) {
            // Concentric-free disc sampling: R2 sequence mapped to a disc (sqrt radius).
            const float u1 = fract(j + 0.7548776662f * static_cast<float>(s));
            const float u2 = fract(j + 0.5698402910f * static_cast<float>(s));
            const float r = std::sqrt(u1) * mask_spread, phi = 2.0f * math::kPi * u2;
            const Vec3 d = math::normalize(mask_l + mask_t * (r * std::cos(phi)) + mask_b * (r * std::sin(phi)));
            ++rays;
            if (!tracer.bvh().occluded(make_ray(origin, d), 1e30f)) ++lit;
        }
        return static_cast<float>(lit) / static_cast<float>(mask_samples);
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
                if (settings.include_direct) e += tracer.direct(t.p, t.n, rays, &dsum) * (1.0f / math::kPi);
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
    w.u32((directional ? kFlagDirectional : 0u) | (shadowmask ? kFlagShadowmask : 0u));
    if (shadowmask) {
        const Vec3 d = math::normalize(file.shadowmask_direction);
        w.f32(d.x);
        w.f32(d.y);
        w.f32(d.z);
    }
    const std::size_t texels = static_cast<std::size_t>(file.atlas.width) * file.atlas.height;
    for (std::size_t i = 0; i < texels * 4; ++i) w.u16(float_to_half(i < file.atlas.pixels.size() ? file.atlas.pixels[i] : 0.0f));
    if (directional) w.bytes(file.direction.pixels.data(), file.direction.pixels.size());
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
    if (version != 1 && version != kLightmapFileVersion)
        return make_error(ErrorCode::VersionMismatch, "unsupported lightmap file version " + std::to_string(version));
    std::uint32_t flags = 0;
    if (version >= 2 && !r.u32(flags)) return make_error(ErrorCode::ParseError, "truncated lightmap header");
    if ((flags & ~(kFlagDirectional | kFlagShadowmask)) != 0) return make_error(ErrorCode::ParseError, "unknown lightmap file flags");
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
