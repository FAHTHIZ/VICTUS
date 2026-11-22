// SPDX-License-Identifier: Apache-2.0
// City meshes (ADR-0202): one vertex-coloured mesh per (cell, layer), flat-shaded.
#include "victus/procgen/city.hpp"

#include <algorithm>
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

struct Builder {
    scene::ProceduralMesh m;
    // Front faces wind counter-clockwise around their normal; `want` is the intended outward
    // direction, and the winding is flipped when the corners were given the other way round.
    void quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, std::uint32_t color, Vec3 want = {0, 1, 0}) {
        Vec3 n = math::normalize(math::cross(b - a, c - a));
        if (math::dot(n, want) < 0.0f) {
            std::swap(b, d);
            n = n * -1.0f;
        }
        const auto base = static_cast<std::uint32_t>(m.positions.size());
        for (const Vec3& p : {a, b, c, d}) {
            m.positions.push_back(p);
            m.normals.push_back(n);
            m.colors.push_back(color);
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
    // Low-poly tree crown: an octahedron stretched upward, eight flat faces.
    void crown(Vec3 c, float r, float h, std::uint32_t color) {
        const Vec3 top{c.x, c.y + h, c.z}, bot{c.x, c.y - h * 0.6f, c.z};
        Vec3 ring[6];
        for (int i = 0; i < 6; ++i) {
            const float a = static_cast<float>(i) * std::numbers::pi_v<float> / 3.0f;
            ring[i] = {c.x + r * std::cos(a), c.y, c.z + r * std::sin(a)};
        }
        for (int i = 0; i < 6; ++i) {
            const Vec3 a = ring[i], b = ring[(i + 1) % 6];
            const Vec3 mid = (a + b) * 0.5f - c;
            tri(a, top, b, shade(color, 1.0f + 0.06f * static_cast<float>(i % 3)), mid + Vec3{0, r, 0});
            tri(b, bot, a, shade(color, 0.75f), mid - Vec3{0, r, 0});
        }
    }
};

// Facade: one band of windows per floor, slightly proud of the wall, on every side longer than 3 m.
void facade(Builder& b, const Rect& fp, float y0, float y1, float floor_h, const Style& st) {
    const float out = 0.03f, margin = 0.6f;
    const std::uint32_t win = st.window;
    for (float f = y0; f + floor_h * 0.5f <= y1; f += floor_h) {
        const float lo = f + floor_h * 0.28f, hi = std::min(f + floor_h * 0.82f, y1 - 0.2f);
        if (hi <= lo) break;
        const float x0 = fp.min.x + margin, x1 = fp.max.x - margin, z0 = fp.min.y + margin, z1 = fp.max.y - margin;
        if (x1 - x0 > 2.0f) {
            b.quad({x0, lo, fp.min.y - out}, {x1, lo, fp.min.y - out}, {x1, hi, fp.min.y - out}, {x0, hi, fp.min.y - out}, win, {0, 0, -1});
            b.quad({x1, lo, fp.max.y + out}, {x0, lo, fp.max.y + out}, {x0, hi, fp.max.y + out}, {x1, hi, fp.max.y + out}, win, {0, 0, 1});
        }
        if (z1 - z0 > 2.0f) {
            b.quad({fp.min.x - out, lo, z1}, {fp.min.x - out, lo, z0}, {fp.min.x - out, hi, z0}, {fp.min.x - out, hi, z1}, win, {-1, 0, 0});
            b.quad({fp.max.x + out, lo, z0}, {fp.max.x + out, lo, z1}, {fp.max.x + out, hi, z1}, {fp.max.x + out, hi, z0}, win, {1, 0, 0});
        }
    }
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
    // Buildings: tiers with window bands and a darker roof; a rooftop unit on big roofs.
    for (const Building& bd : city.buildings) {
        const Style& st = kStyles[bd.style % kStyleCount];
        Builder& b = at(bd.lot.center(), CityLayer::Buildings);
        float y0 = s.kerb_height;
        for (std::size_t t = 0; t < bd.tiers.size(); ++t) {
            const Tier& tier = bd.tiers[t];
            const float y1 = s.kerb_height + tier.top;
            const Rect& f = tier.footprint;
            b.box({f.min.x, y0, f.min.y}, {f.max.x, y1, f.max.y}, shade(st.wall, 1.0f - 0.04f * static_cast<float>(t)), st.roof);
            facade(b, f, y0, y1, s.floor_height, st);
            y0 = y1;
        }
        const Rect& roof = bd.tiers.back().footprint;
        if (roof.area() > 120.0f) {
            const Vec2 c = roof.center();
            b.box({c.x - 2.0f, y0, c.y - 1.5f}, {c.x + 2.0f, y0 + 1.8f, c.y + 1.5f}, shade(st.roof, 1.2f), shade(st.roof, 1.1f));
        }
    }
    // Street lights (pole, arm over the road, lamp head) and trees (trunk + crown).
    for (const Prop& p : city.props) {
        const Vec2 xz{p.position.x, p.position.z};
        if (p.kind == PropKind::StreetLight) {
            Builder& b = at(xz, CityLayer::Props);
            b.yawed_box(p.position, p.yaw, {-0.09f, 0.0f, -0.09f}, {0.09f, 7.0f, 0.09f}, kPole, kPole);
            b.yawed_box(p.position, p.yaw, {-0.05f, 6.8f, 0.0f}, {0.05f, 6.92f, 1.9f}, kPole, kPole);
            b.yawed_box(p.position, p.yaw, {-0.22f, 6.62f, 1.6f}, {0.22f, 6.8f, 2.1f}, kLamp, kPole);
        } else {
            at(xz, CityLayer::Props).yawed_box(p.position, p.yaw, {-0.14f * p.scale, 0.0f, -0.14f * p.scale},
                                                {0.14f * p.scale, 2.6f * p.scale, 0.14f * p.scale}, kTrunk, kTrunk);
            const std::uint32_t leaf = shade(rgb(66, 108, 48), 0.85f + 0.3f * std::fmod(p.yaw, 1.0f));
            at(xz, CityLayer::Foliage).crown({p.position.x, p.position.y + 3.6f * p.scale, p.position.z}, 2.0f * p.scale, 2.0f * p.scale, leaf);
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
