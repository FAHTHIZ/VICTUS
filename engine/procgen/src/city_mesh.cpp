// SPDX-License-Identifier: Apache-2.0
// City meshes (ADR-0202): one vertex-coloured mesh per (cell, layer), flat-shaded.
#include "victus/procgen/city.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numbers>
#include <tuple>

namespace victus::procgen {

using math::Vec2;
using math::Vec3;

namespace {

constexpr std::uint32_t rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return std::uint32_t{r} | (std::uint32_t{g} << 8) | (std::uint32_t{b} << 16) | (0xffu << 24);
}
std::uint32_t shade(std::uint32_t c, float f) {
    const auto ch = [&](int k) {
        return static_cast<std::uint32_t>(std::clamp(static_cast<float>((c >> (k * 8)) & 0xff) * f, 0.0f, 255.0f)) << (k * 8);
    };
    return ch(0) | ch(1) | ch(2) | (0xffu << 24);
}

// Facade, window and roof colours per building style (sRGB).
struct Style {
    std::uint32_t wall, window, roof;
};
constexpr Style kStyles[kStyleCount] = {
    {rgb(176, 160, 138), rgb(52, 60, 70), rgb(92, 88, 84)},   // sandstone
    {rgb(150, 84, 64), rgb(44, 50, 58), rgb(78, 70, 66)},     // brick
    {rgb(196, 196, 190), rgb(58, 70, 84), rgb(110, 110, 108)}, // white render
    {rgb(118, 122, 128), rgb(40, 48, 58), rgb(84, 86, 90)},   // concrete
    {rgb(92, 118, 140), rgb(46, 74, 102), rgb(70, 76, 84)},   // blue glass tower
    {rgb(64, 72, 78), rgb(96, 120, 132), rgb(58, 62, 66)},    // dark glass tower
};
constexpr std::uint32_t kAsphalt = rgb(46, 48, 52), kMarking = rgb(220, 220, 210), kYellow = rgb(222, 180, 40);
constexpr std::uint32_t kSidewalk = rgb(150, 148, 142), kKerb = rgb(120, 118, 114), kGrass = rgb(70, 112, 52);
constexpr std::uint32_t kPole = rgb(60, 64, 68), kLamp = rgb(250, 236, 196), kTrunk = rgb(84, 64, 46);

// Deterministic per-feature randomness for colour and detail variation.
std::uint32_t hash32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}
float unit(std::uint32_t h) { return static_cast<float>(hash32(h) >> 8) / 16777216.0f; }
std::uint32_t point_hash(Vec2 p, std::uint32_t salt) {
    const auto qx = static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(p.x * 8.0f)));
    const auto qz = static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(p.y * 8.0f)));
    return hash32(qx * 0x9E3779B1u ^ hash32(qz + salt * 0x85EBCA77u));
}
// Per-channel tint: brightness `f` and a small warm/cool shift `w` in [-1, 1].
std::uint32_t tint(std::uint32_t c, float f, float w) {
    const float k[3] = {f * (1.0f + 0.05f * w), f, f * (1.0f - 0.05f * w)};
    std::uint32_t out = 0xffu << 24;
    for (int i = 0; i < 3; ++i)
        out |= static_cast<std::uint32_t>(std::clamp(static_cast<float>((c >> (i * 8)) & 0xff) * k[i], 0.0f, 255.0f)) << (i * 8);
    return out;
}

struct Builder {
    scene::ProceduralMesh m;
    // Front faces wind counter-clockwise around their normal; `want` is the intended outward
    // direction, and the winding is flipped when the corners were given the other way round.
    void quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, std::uint32_t color, Vec3 want = {0, 1, 0}) { quad4(a, b, c, d, {color, color, color, color}, want); }
    // Quad with one colour per corner (a, b, c, d), for vertical gradients.
    void quad4(Vec3 a, Vec3 b, Vec3 c, Vec3 d, std::array<std::uint32_t, 4> col, Vec3 want) {
        Vec3 n = math::normalize(math::cross(b - a, c - a));
        if (math::dot(n, want) < 0.0f) {
            std::swap(b, d);
            std::swap(col[1], col[3]);
            n = n * -1.0f;
        }
        const auto base = static_cast<std::uint32_t>(m.positions.size());
        const Vec3 ps[4] = {a, b, c, d};
        for (int i = 0; i < 4; ++i) {
            m.positions.push_back(ps[i]);
            m.normals.push_back(n);
            m.colors.push_back(col[static_cast<std::size_t>(i)]);
        }
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    void tri(Vec3 a, Vec3 b, Vec3 c, std::uint32_t color, Vec3 want) {
        Vec3 n = math::normalize(math::cross(b - a, c - a));
        if (math::dot(n, want) < 0.0f) {
            std::swap(b, c);
            n = n * -1.0f;
        }
        const auto base = static_cast<std::uint32_t>(m.positions.size());
        for (const Vec3& p : {a, b, c}) {
            m.positions.push_back(p);
            m.normals.push_back(n);
            m.colors.push_back(color);
        }
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2});
    }
    // Horizontal rectangle facing up at height y.
    void flat(Vec2 lo, Vec2 hi, float y, std::uint32_t color) {
        quad({lo.x, y, lo.y}, {lo.x, y, hi.y}, {hi.x, y, hi.y}, {hi.x, y, lo.y}, color);
    }
    // Axis-aligned box without its bottom face.
    void box(Vec3 lo, Vec3 hi, std::uint32_t side, std::uint32_t top) {
        flat({lo.x, lo.z}, {hi.x, hi.z}, hi.y, top);
        quad({lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z}, side, {0, 0, -1});
        quad({hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {hi.x, hi.y, hi.z}, side, {0, 0, 1});
        quad({lo.x, lo.y, hi.z}, {lo.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {lo.x, hi.y, hi.z}, side, {-1, 0, 0});
        quad({hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {hi.x, hi.y, lo.z}, side, {1, 0, 0});
    }
    // Box without its bottom face whose sides fade from `bottom` at lo.y to `side` at hi.y.
    void box_grad(Vec3 lo, Vec3 hi, std::uint32_t bottom, std::uint32_t side, std::uint32_t top) {
        flat({lo.x, lo.z}, {hi.x, hi.z}, hi.y, top);
        const std::array<std::uint32_t, 4> g{bottom, bottom, side, side};
        quad4({lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z}, g, {0, 0, -1});
        quad4({hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {hi.x, hi.y, hi.z}, g, {0, 0, 1});
        quad4({lo.x, lo.y, hi.z}, {lo.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {lo.x, hi.y, hi.z}, g, {-1, 0, 0});
        quad4({hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {hi.x, hi.y, lo.z}, g, {1, 0, 0});
    }
    // Box rotated by yaw about +Y around `origin` (local x right, z forward).
    void yawed_box(Vec3 origin, float yaw, Vec3 lo, Vec3 hi, std::uint32_t side, std::uint32_t top) {
        const float c = std::cos(yaw), s = std::sin(yaw);
        const auto P = [&](float x, float y, float z) { return Vec3{origin.x + x * c + z * s, origin.y + y, origin.z - x * s + z * c}; };
        const auto D = [&](float x, float z) { return Vec3{x * c + z * s, 0, -x * s + z * c}; }; // rotated direction
        quad(P(lo.x, hi.y, lo.z), P(lo.x, hi.y, hi.z), P(hi.x, hi.y, hi.z), P(hi.x, hi.y, lo.z), top, {0, 1, 0});
        quad(P(lo.x, lo.y, lo.z), P(hi.x, lo.y, lo.z), P(hi.x, hi.y, lo.z), P(lo.x, hi.y, lo.z), side, D(0, -1));
        quad(P(hi.x, lo.y, hi.z), P(lo.x, lo.y, hi.z), P(lo.x, hi.y, hi.z), P(hi.x, hi.y, hi.z), side, D(0, 1));
        quad(P(lo.x, lo.y, hi.z), P(lo.x, lo.y, lo.z), P(lo.x, hi.y, lo.z), P(lo.x, hi.y, hi.z), side, D(-1, 0));
        quad(P(hi.x, lo.y, lo.z), P(hi.x, lo.y, hi.z), P(hi.x, hi.y, hi.z), P(hi.x, hi.y, lo.z), side, D(1, 0));
        quad(P(lo.x, lo.y, hi.z), P(hi.x, lo.y, hi.z), P(hi.x, lo.y, lo.z), P(lo.x, lo.y, lo.z), side, {0, -1, 0}); // lamp heads are seen from below
    }
    // Leaf clump: a lumpy low-poly sphere with smooth radial normals (soft shading rather than
    // flat facets), lighter on top and darker underneath. 7 segments x 4 bands, 56 triangles.
    void blob(Vec3 c, float r, float h, std::uint32_t color, std::uint32_t seed) {
        constexpr int kSeg = 7, kBand = 4;
        const auto base = static_cast<std::uint32_t>(m.positions.size());
        const auto vert = [&](Vec3 dir, float lump) {
            m.positions.push_back({c.x + dir.x * r * lump, c.y + dir.y * h * lump, c.z + dir.z * r * lump});
            m.normals.push_back(math::normalize(Vec3{dir.x / r, dir.y / h, dir.z / r}));
            m.colors.push_back(shade(color, 0.62f + 0.5f * (dir.y * 0.5f + 0.5f)));
        };
        vert({0, 1, 0}, 1.0f);
        for (int j = 1; j < kBand; ++j) {
            const float phi = std::numbers::pi_v<float> * static_cast<float>(j) / kBand;
            for (int i = 0; i < kSeg; ++i) {
                const float th = 2.0f * std::numbers::pi_v<float> * (static_cast<float>(i) + 0.5f * static_cast<float>(j & 1)) / kSeg;
                const float lump = 0.88f + 0.24f * unit(seed * 97u + static_cast<std::uint32_t>(j * kSeg + i));
                vert({std::sin(phi) * std::cos(th), std::cos(phi), std::sin(phi) * std::sin(th)}, lump);
            }
        }
        vert({0, -1, 0}, 1.0f);
        const auto ring = [&](int j, int i) { return base + 1u + static_cast<std::uint32_t>((j - 1) * kSeg + (i % kSeg)); };
        const std::uint32_t bottom = base + 1u + static_cast<std::uint32_t>((kBand - 1) * kSeg);
        for (int i = 0; i < kSeg; ++i) m.indices.insert(m.indices.end(), {base, ring(1, i + 1), ring(1, i)});
        for (int j = 1; j + 1 < kBand; ++j)
            for (int i = 0; i < kSeg; ++i) {
                // Odd bands are rotated half a segment; pair each vertex with its neighbours below.
                const int k = (j & 1) ? i + 1 : i;
                m.indices.insert(m.indices.end(), {ring(j, i), ring(j, i + 1), ring(j + 1, k)});
                m.indices.insert(m.indices.end(), {ring(j, i), ring(j + 1, k), ring(j + 1, k + kSeg - 1)});
            }
        for (int i = 0; i < kSeg; ++i) m.indices.insert(m.indices.end(), {bottom, ring(kBand - 1, i), ring(kBand - 1, i + 1)});
    }
};

constexpr std::uint32_t kAwnings[] = {rgb(150, 44, 40), rgb(38, 86, 70), rgb(40, 62, 110), rgb(196, 150, 60), rgb(70, 70, 74)};

// One wall of a tier: from `a` to `b` along the ground (outward normal `o`), between heights y0
// and y1. Windows are glass bands per floor on the glass layer, split into separate windows by
// wall-coloured piers (masonry) or thin frame mullions (towers); masonry ground floors are
// shopfronts under an awning.
struct Wall {
    Vec2 a, b, o;
};
void facade_wall(Builder& wall, Builder& glass, const Wall& w, float y0, float y1, float floor_h, const Style& st,
                 std::uint32_t pier_color, bool tower, bool shopfront, std::uint32_t seed) {
    const Vec2 d = w.b - w.a;
    const float len = std::sqrt(d.x * d.x + d.y * d.y);
    const float margin = tower ? 0.25f : 0.6f;
    if (len - 2.0f * margin < 2.0f) return;
    const Vec2 t = d / len;
    const Vec3 n{w.o.x, 0, w.o.y};
    const auto P = [&](float s, float y, float out) {
        const Vec2 q = w.a + t * s + w.o * out;
        return Vec3{q.x, y, q.y};
    };
    const float s0 = margin, s1 = len - margin;
    const float glass_out = 0.03f, pier_out = tower ? 0.05f : 0.08f;
    float first = y0;
    if (shopfront && y1 - y0 > floor_h * 1.5f) {
        // Shop units 5-8 m wide, each with its own window, awning colour and a pier between.
        const float top = y0 + floor_h * 0.78f;
        const auto units = std::max(1, static_cast<int>(std::floor((s1 - s0) / (5.0f + 3.0f * unit(seed + 21u)))));
        const float uw = (s1 - s0) / static_cast<float>(units);
        for (int u = 0; u < units; ++u) {
            const float a0 = s0 + uw * static_cast<float>(u) + 0.3f, a1 = a0 + uw - 0.6f;
            const auto us = seed * 7u + static_cast<std::uint32_t>(u);
            glass.quad(P(a0, y0 + 0.35f, glass_out), P(a1, y0 + 0.35f, glass_out), P(a1, top, glass_out), P(a0, top, glass_out),
                       shade(st.window, 0.75f + 0.3f * unit(us)), n);
            if (unit(us + 3u) < 0.75f) { // most shops have an awning
                const std::uint32_t aw = kAwnings[hash32(us) % std::size(kAwnings)];
                const Vec3 l0 = P(a0, top, 0.0f), l1 = P(a1, top, 0.9f);
                wall.box({std::min(l0.x, l1.x), top, std::min(l0.z, l1.z)}, {std::max(l0.x, l1.x), top + 0.32f, std::max(l0.z, l1.z)}, aw,
                         shade(aw, 0.8f));
            }
            if (u > 0) {
                const float c = s0 + uw * static_cast<float>(u);
                wall.quad(P(c - 0.3f, y0, 0.08f), P(c + 0.3f, y0, 0.08f), P(c + 0.3f, y0 + floor_h, 0.08f), P(c - 0.3f, y0 + floor_h, 0.08f),
                          pier_color, n);
            }
        }
        first = y0 + floor_h;
    }
    int floor = 0;
    for (float f = first; f + floor_h * 0.5f <= y1; f += floor_h, ++floor) {
        const float lo = f + floor_h * (tower ? 0.1f : 0.28f), hi = std::min(f + floor_h * (tower ? 0.94f : 0.82f), y1 - 0.2f);
        if (hi <= lo) break;
        const float v = 0.82f + 0.36f * unit(seed * 31u + static_cast<std::uint32_t>(floor));
        glass.quad(P(s0, lo, glass_out), P(s1, lo, glass_out), P(s1, hi, glass_out), P(s0, hi, glass_out), shade(st.window, v), n);
    }
    // Piers / mullions span the whole wall, dividing the bands into windows.
    const float bay = tower ? 1.6f : 2.4f + 1.0f * unit(seed + 7u);
    const float pier = tower ? 0.1f : 0.42f;
    const auto bays = std::max(1, static_cast<int>(std::floor((s1 - s0) / bay)));
    const float step = (s1 - s0) / static_cast<float>(bays);
    const float py0 = first, py1 = y1 - 0.15f;
    if (py1 <= py0) return;
    for (int i = 1; i < bays; ++i) {
        const float c = s0 + step * static_cast<float>(i);
        wall.quad(P(c - pier * 0.5f, py0, pier_out), P(c + pier * 0.5f, py0, pier_out), P(c + pier * 0.5f, py1, pier_out),
                  P(c - pier * 0.5f, py1, pier_out), pier_color, n);
    }
}

void facade(Builder& wall, Builder& glass, const Rect& fp, float y0, float y1, float floor_h, const Style& st, std::uint32_t pier_color,
            bool tower, bool shopfront, std::uint32_t seed) {
    const Wall walls[4] = {
        {{fp.min.x, fp.min.y}, {fp.max.x, fp.min.y}, {0, -1}},
        {{fp.max.x, fp.max.y}, {fp.min.x, fp.max.y}, {0, 1}},
        {{fp.min.x, fp.max.y}, {fp.min.x, fp.min.y}, {-1, 0}},
        {{fp.max.x, fp.min.y}, {fp.max.x, fp.max.y}, {1, 0}},
    };
    for (std::uint32_t i = 0; i < 4; ++i) facade_wall(wall, glass, walls[i], y0, y1, floor_h, st, pier_color, tower, shopfront, seed * 4u + i);
}

// Dashed line along a road segment (from a to b on the ground), offset sideways by `side`.
void dashes(Builder& b, Vec2 a, Vec2 bb, float side, float width, float dash, float gap, std::uint32_t color) {
    const Vec2 d = bb - a;
    const float len = std::sqrt(d.x * d.x + d.y * d.y);
    if (len < 1.0f) return;
    const Vec2 t = d / len, n{-t.y, t.x};
    const float y = 0.012f;
    const float step = gap > 0.0f ? dash + gap : len;
    for (float s = gap > 0.0f ? gap * 0.5f : 0.0f; s < len; s += step) {
        const float e = std::min(s + (gap > 0.0f ? dash : len), len);
        const Vec2 p0 = a + t * s + n * (side - width * 0.5f), p1 = a + t * e + n * (side - width * 0.5f);
        const Vec2 p2 = a + t * e + n * (side + width * 0.5f), p3 = a + t * s + n * (side + width * 0.5f);
        b.quad({p0.x, y, p0.y}, {p3.x, y, p3.y}, {p2.x, y, p2.y}, {p1.x, y, p1.y}, color);
    }
}

// Lane markings and crossings of one road segment between two intersections.
void road_segment(Builder& b, Vec2 a, Vec2 e, float width, std::uint32_t lanes, float lane_w) {
    const Vec2 d = e - a;
    const float len = std::sqrt(d.x * d.x + d.y * d.y);
    if (len < 8.0f) return;
    const Vec2 t = d / len;
    // Leave room for the zebra crossings at both ends.
    const Vec2 s0 = a + t * 4.0f, s1 = e - t * 4.0f;
    if (lanes >= 2) {
        dashes(b, s0, s1, -0.12f, 0.12f, 0, 0, kYellow); // double yellow centre
        dashes(b, s0, s1, 0.12f, 0.12f, 0, 0, kYellow);
        for (std::uint32_t l = 1; l < lanes; ++l) {
            dashes(b, s0, s1, lane_w * static_cast<float>(l), 0.12f, 3.0f, 6.0f, kMarking);
            dashes(b, s0, s1, -lane_w * static_cast<float>(l), 0.12f, 3.0f, 6.0f, kMarking);
        }
    } else {
        dashes(b, s0, s1, 0.0f, 0.12f, 3.0f, 6.0f, kMarking);
    }
    // Zebra crossings: stripes along the road direction across its width.
    const Vec2 n{-t.y, t.x};
    for (const auto& [at, dir] : {std::pair{a, t}, std::pair{e, t * -1.0f}}) {
        for (float o = -width * 0.5f + 0.6f; o < width * 0.5f - 0.4f; o += 1.0f) {
            const Vec2 c = at + dir * 0.5f + n * o;
            const Vec2 p0 = c, p1 = c + dir * 3.0f, p2 = c + dir * 3.0f + n * 0.5f, p3 = c + n * 0.5f;
            b.quad({p0.x, 0.012f, p0.y}, {p3.x, 0.012f, p3.y}, {p2.x, 0.012f, p2.y}, {p1.x, 0.012f, p1.y}, kMarking);
        }
    }
}

} // namespace

std::vector<CityMesh> build_city_meshes(const City& city, float cell_size) {
    const CitySettings& s = city.settings;
    const Vec2 origin{city.bounds.min.x, city.bounds.min.z};
    const bool one = !(cell_size > 0.0f);
    std::map<std::tuple<int, int, int>, Builder> cells;
    const auto at = [&](Vec2 p, CityLayer layer) -> Builder& {
        const int cx = one ? 0 : static_cast<int>(std::floor((p.x - origin.x) / cell_size));
        const int cz = one ? 0 : static_cast<int>(std::floor((p.y - origin.y) / cell_size));
        return cells[{cx, cz, static_cast<int>(layer)}];
    };
    const std::size_t nx = city.xs.size(), nz = city.zs.size();
    // Asphalt: intersections, then the segments between them (with markings).
    for (std::size_t j = 0; j < nz; ++j)
        for (std::size_t i = 0; i < nx; ++i) {
            const Vec2 c{city.xs[i], city.zs[j]};
            const float hx = city.x_width[i] * 0.5f, hz = city.z_width[j] * 0.5f;
            at(c, CityLayer::Ground).flat({c.x - hx, c.y - hz}, {c.x + hx, c.y + hz}, 0.0f, kAsphalt);
            if (i + 1 < nx) { // along X on z-line j
                const Vec2 a{c.x + hx, c.y}, e{city.xs[i + 1] - city.x_width[i + 1] * 0.5f, c.y};
                Builder& b = at((a + e) * 0.5f, CityLayer::Ground);
                b.flat({a.x, c.y - hz}, {e.x, c.y + hz}, 0.0f, kAsphalt);
                road_segment(b, a, e, city.z_width[j], static_cast<std::uint32_t>(std::lround(hz / s.lane_width)), s.lane_width);
            }
            if (j + 1 < nz) { // along Z on x-line i
                const Vec2 a{c.x, c.y + hz}, e{c.x, city.zs[j + 1] - city.z_width[j + 1] * 0.5f};
                Builder& b = at((a + e) * 0.5f, CityLayer::Ground);
                b.flat({c.x - hx, a.y}, {c.x + hx, e.y}, 0.0f, kAsphalt);
                road_segment(b, a, e, city.x_width[i], static_cast<std::uint32_t>(std::lround(hx / s.lane_width)), s.lane_width);
            }
        }
    // Kerb plates (sidewalk ring and block interior), parks.
    for (const Block& bl : city.blocks) {
        Builder& b = at(bl.kerb.center(), CityLayer::Ground);
        b.box({bl.kerb.min.x, 0.0f, bl.kerb.min.y}, {bl.kerb.max.x, s.kerb_height, bl.kerb.max.y}, kKerb, kSidewalk);
        if (bl.park) b.flat(bl.inner.min, bl.inner.max, s.kerb_height + 0.01f, kGrass);
    }
    // Buildings: tiers with windows, piers, cornices and a darker roof; per-building tint, a
    // darker band of grime at street level, shopfronts on masonry ground floors and rooftop units.
    for (const Building& bd : city.buildings) {
        const Style& st = kStyles[bd.style % kStyleCount];
        const std::uint32_t seed = point_hash(bd.lot.center(), bd.style);
        const bool tower = bd.style >= 4;
        const std::uint32_t wall = tint(st.wall, 0.86f + 0.26f * unit(seed), unit(seed + 1u) * 2.0f - 1.0f);
        const std::uint32_t roof = tint(st.roof, 0.85f + 0.3f * unit(seed + 2u), 0.0f);
        const std::uint32_t pier = tower ? tint(rgb(150, 156, 162), 0.8f + 0.3f * unit(seed + 3u), 0.0f) : shade(wall, 1.06f);
        const std::uint32_t trim = tower ? shade(pier, 0.9f) : shade(wall, 0.8f);
        Builder& b = at(bd.lot.center(), CityLayer::Buildings);
        Builder& g = at(bd.lot.center(), CityLayer::Glass);
        float y0 = s.kerb_height;
        for (std::size_t t = 0; t < bd.tiers.size(); ++t) {
            const Tier& tier = bd.tiers[t];
            const float y1 = s.kerb_height + tier.top;
            const Rect& f = tier.footprint;
            const std::uint32_t side = shade(wall, 1.0f - 0.04f * static_cast<float>(t));
            b.box_grad({f.min.x, y0, f.min.y}, {f.max.x, y1, f.max.y}, t == 0 ? shade(side, 0.72f) : side, side, roof);
            facade(b, g, f, y0, y1, s.floor_height, st, pier, tower, t == 0 && !tower && bd.floors >= 2, seed + static_cast<std::uint32_t>(t) * 13u);
            // Cornice / parapet lip around the top of the tier.
            const float lip = tower ? 0.12f : 0.28f, ch = tower ? 0.5f : 0.7f;
            b.box({f.min.x - lip, y1 - ch, f.min.y - lip}, {f.max.x + lip, y1 - 0.02f, f.max.y + lip}, trim, shade(trim, 0.9f));
            y0 = y1;
        }
        const Rect& top = bd.tiers.back().footprint;
        if (top.area() > 120.0f) { // rooftop plant: one to three units off-centre
            const Vec2 c = top.center(), half = top.size() * 0.5f;
            const int units = 1 + static_cast<int>(hash32(seed + 5u) % 3u);
            for (int u = 0; u < units; ++u) {
                const auto us = seed + 11u * static_cast<std::uint32_t>(u + 1);
                const Vec2 sz{std::min(1.2f + 2.0f * unit(us + 2u), half.x - 1.0f), std::min(1.0f + 1.6f * unit(us + 3u), half.y - 1.0f)};
                const Vec2 off{(unit(us) - 0.5f) * 2.0f * std::max(half.x - sz.x - 1.0f, 0.0f),
                               (unit(us + 1u) - 0.5f) * 2.0f * std::max(half.y - sz.y - 1.0f, 0.0f)};
                const float h = 1.0f + 1.6f * unit(us + 4u);
                const Vec2 p = c + off;
                b.box({p.x - sz.x, y0, p.y - sz.y}, {p.x + sz.x, y0 + h, p.y + sz.y}, shade(roof, 1.25f), shade(roof, 1.12f));
            }
        }
    }
    // Street lights (pole, arm over the road, lamp head) and trees (trunk + two or three soft clumps).
    for (const Prop& p : city.props) {
        const Vec2 xz{p.position.x, p.position.z};
        if (p.kind == PropKind::StreetLight) {
            Builder& b = at(xz, CityLayer::Props);
            b.yawed_box(p.position, p.yaw, {-0.09f, 0.0f, -0.09f}, {0.09f, 7.0f, 0.09f}, kPole, kPole);
            b.yawed_box(p.position, p.yaw, {-0.05f, 6.8f, 0.0f}, {0.05f, 6.92f, 1.9f}, kPole, kPole);
            b.yawed_box(p.position, p.yaw, {-0.22f, 6.62f, 1.6f}, {0.22f, 6.8f, 2.1f}, kLamp, kPole);
        } else {
            const std::uint32_t seed = point_hash(xz, 3u);
            const float sc = p.scale;
            at(xz, CityLayer::Props).yawed_box(p.position, p.yaw, {-0.13f * sc, 0.0f, -0.13f * sc}, {0.13f * sc, 3.0f * sc, 0.13f * sc}, kTrunk,
                                                kTrunk);
            const std::uint32_t leaf = tint(rgb(72, 110, 50), 0.8f + 0.35f * unit(seed), unit(seed + 1u) * 2.0f - 1.0f);
            Builder& fb = at(xz, CityLayer::Foliage);
            const Vec3 c{p.position.x, p.position.y + 3.9f * sc, p.position.z};
            fb.blob(c, 1.9f * sc, 1.6f * sc, leaf, seed);
            const int extra = 1 + static_cast<int>(hash32(seed + 2u) % 2u);
            for (int k = 0; k < extra; ++k) {
                const float a = p.yaw + 2.4f * static_cast<float>(k) + unit(seed + 3u + static_cast<std::uint32_t>(k));
                const float r = (1.1f + 0.4f * unit(seed + 7u + static_cast<std::uint32_t>(k))) * sc;
                const Vec3 o{std::cos(a) * r, (0.3f + 0.7f * unit(seed + 9u + static_cast<std::uint32_t>(k))) * sc, std::sin(a) * r};
                fb.blob(c + o, 1.25f * sc, 1.1f * sc, shade(leaf, 0.92f + 0.16f * unit(seed + 13u)), seed + 17u * static_cast<std::uint32_t>(k + 1));
            }
        }
    }
    std::vector<CityMesh> out;
    out.reserve(cells.size());
    for (auto& [key, b] : cells) {
        if (b.m.indices.empty()) continue;
        CityMesh cm;
        cm.cell = {static_cast<float>(std::get<0>(key)), static_cast<float>(std::get<1>(key))};
        cm.layer = static_cast<CityLayer>(std::get<2>(key));
        cm.mesh = std::move(b.m);
        for (const Vec3& p : cm.mesh.positions) cm.mesh.bounds.expand(p);
        out.push_back(std::move(cm));
    }
    return out;
}

} // namespace victus::procgen
