// SPDX-License-Identifier: Apache-2.0
// City layout generation (ADR-0202): roads, blocks, lots, buildings, props and walking loops.
#include "victus/procgen/city.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace victus::procgen {

using math::Vec2;
using math::Vec3;

namespace {

// SplitMix64: tiny, fast and identical on every platform (std distributions are not).
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {}
    std::uint64_t next() noexcept {
        std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float unit() noexcept { return static_cast<float>(next() >> 40) * (1.0f / 16777216.0f); } // [0, 1)
    float range(float a, float b) noexcept { return a + (b - a) * unit(); }
    std::uint32_t below(std::uint32_t n) noexcept { return n ? static_cast<std::uint32_t>(next() % n) : 0; }
};

Rect inset(const Rect& r, float d) { return {{r.min.x + d, r.min.y + d}, {r.max.x - d, r.max.y - d}}; }
bool usable(const Rect& r, float min_side) { return r.max.x - r.min.x >= min_side && r.max.y - r.min.y >= min_side; }

// Splits [a, b] into pieces of length in [lo, hi]; a short remainder joins the last piece.
std::vector<std::pair<float, float>> split(float a, float b, float lo, float hi, Rng& rng) {
    std::vector<std::pair<float, float>> out;
    float at = a;
    while (b - at >= lo) {
        const float len = std::min(rng.range(lo, hi), b - at);
        out.emplace_back(at, at + len);
        at += len;
    }
    if (!out.empty()) out.back().second = b;
    else if (b - a > 0.5f) out.emplace_back(a, b);
    return out;
}

// A strip of lots along one side of a block. `along_x`: the frontage runs along X.
void lots_in_strip(const Rect& strip, bool along_x, const CitySettings& s, Rng& rng, std::vector<Rect>& out) {
    const float a = along_x ? strip.min.x : strip.min.y, b = along_x ? strip.max.x : strip.max.y;
    for (const auto& [lo, hi] : split(a, b, s.lot_min, s.lot_max, rng)) {
        Rect r = strip;
        if (along_x) {
            r.min.x = lo;
            r.max.x = hi;
        } else {
            r.min.y = lo;
            r.max.y = hi;
        }
        out.push_back(r);
    }
}

} // namespace

math::Vec3 WalkLoop::point_at(float s, math::Vec3* tangent) const {
    if (points.size() < 2 || !(length > 0.0f)) {
        if (tangent) *tangent = {0, 0, 1};
        return points.empty() ? Vec3{} : points.front();
    }
    float t = std::fmod(s, length);
    if (t < 0.0f) t += length;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const Vec3 a = points[i], b = points[(i + 1) % points.size()];
        const float seg = math::length(b - a);
        if (t <= seg || i + 1 == points.size()) {
            const Vec3 d = seg > 1e-6f ? (b - a) / seg : Vec3{0, 0, 1};
            if (tangent) *tangent = d;
            return a + d * std::min(t, seg);
        }
        t -= seg;
    }
    return points.front();
}

std::uint32_t City::floors() const noexcept {
    std::uint32_t n = 0;
    for (const Building& b : buildings) n += b.floors;
    return n;
}

Result<City> generate_city(const CitySettings& s) {
    const auto bad = [](std::string m) { return make_error(ErrorCode::InvalidArgument, "city: " + m); };
    if (s.columns == 0 || s.rows == 0 || s.columns > 64 || s.rows > 64) return bad("columns and rows must be 1..64");
    if (!(s.block_min > 0.0f) || s.block_max < s.block_min) return bad("block_min must be positive and <= block_max");
    if (s.lanes == 0 || s.lanes > 4 || s.avenue_lanes == 0 || s.avenue_lanes > 4) return bad("lanes must be 1..4");
    if (!(s.lane_width > 1.0f) || !(s.sidewalk >= 0.0f) || !(s.kerb_height >= 0.0f)) return bad("lane_width, sidewalk or kerb_height out of range");
    if (!(s.lot_min > 2.0f) || s.lot_max < s.lot_min || !(s.lot_depth > 2.0f)) return bad("lot sizes out of range");
    if (!(s.floor_height > 1.0f) || s.floors_min == 0 || s.floors_max < s.floors_min || s.floors_max > 200)
        return bad("floors out of range");

    City c;
    c.settings = s;
    Rng rng(s.seed);
    const auto avenue = [&](std::uint32_t i) { return s.avenue_every > 0 && i % s.avenue_every == 0; };
    const auto lines = [&](std::uint32_t n, std::vector<float>& pos, std::vector<float>& width) {
        pos.assign(n + 1, 0.0f);
        width.assign(n + 1, 0.0f);
        for (std::uint32_t i = 0; i < n; ++i) pos[i + 1] = pos[i] + rng.range(s.block_min, s.block_max);
        const float mid = pos.back() * 0.5f;
        for (std::uint32_t i = 0; i <= n; ++i) {
            pos[i] -= mid;
            width[i] = 2.0f * static_cast<float>(avenue(i) ? s.avenue_lanes : s.lanes) * s.lane_width;
        }
    };
    lines(s.columns, c.xs, c.x_width);
    lines(s.rows, c.zs, c.z_width);

    // Road network: one node per intersection, roads between neighbours.
    const std::uint32_t nx = s.columns + 1, nz = s.rows + 1;
    for (std::uint32_t j = 0; j < nz; ++j)
        for (std::uint32_t i = 0; i < nx; ++i) c.roads.add_node({c.xs[i], 0.0f, c.zs[j]});
    const auto node = [&](std::uint32_t i, std::uint32_t j) { return j * nx + i; };
    for (std::uint32_t j = 0; j < nz; ++j)
        for (std::uint32_t i = 0; i < nx; ++i) {
            if (i + 1 < nx) { // along X, on z-line j
                const std::uint32_t l = avenue(j) ? s.avenue_lanes : s.lanes;
                c.roads.add_road({.a = node(i, j), .b = node(i + 1, j), .lanes = l, .lanes_back = l, .lane_width = s.lane_width,
                                  .speed_limit = s.speed_limit * (avenue(j) ? 1.25f : 1.0f)});
            }
            if (j + 1 < nz) { // along Z, on x-line i
                const std::uint32_t l = avenue(i) ? s.avenue_lanes : s.lanes;
                c.roads.add_road({.a = node(i, j), .b = node(i, j + 1), .lanes = l, .lanes_back = l, .lane_width = s.lane_width,
                                  .speed_limit = s.speed_limit * (avenue(i) ? 1.25f : 1.0f)});
            }
        }

    const float half_diag = 0.5f * std::hypot(c.xs.back() - c.xs.front(), c.zs.back() - c.zs.front());
    const float downtown = s.downtown_radius > 0.0f ? s.downtown_radius : 0.4f * half_diag;
    float top = s.kerb_height;
    for (std::uint32_t j = 0; j < s.rows; ++j)
        for (std::uint32_t i = 0; i < s.columns; ++i) {
            Block b;
            b.kerb = {{c.xs[i] + c.x_width[i] * 0.5f, c.zs[j] + c.z_width[j] * 0.5f},
                      {c.xs[i + 1] - c.x_width[i + 1] * 0.5f, c.zs[j + 1] - c.z_width[j + 1] * 0.5f}};
            b.inner = inset(b.kerb, s.sidewalk);
            if (!usable(b.inner, 4.0f))
                return bad(std::format("block {},{} has no room inside its roads and sidewalks (raise block_min)", i, j));
            b.park = rng.unit() < s.park_chance;
            const auto block_index = static_cast<std::uint32_t>(c.blocks.size());
            c.blocks.push_back(b);

            // Walking loop on the sidewalk centre line, corners rounded (radius ~ sidewalk / 2).
            {
                const Rect w = inset(b.kerb, s.sidewalk * 0.5f);
                const float r = std::min({s.sidewalk * 0.5f + 0.5f, w.size().x * 0.25f, w.size().y * 0.25f});
                const float y = s.kerb_height;
                WalkLoop loop;
                const Vec2 corners[4] = {{w.max.x, w.min.y}, {w.max.x, w.max.y}, {w.min.x, w.max.y}, {w.min.x, w.min.y}};
                const float start[4] = {-0.5f, 0.0f, 0.5f, 1.0f}; // arc start angles / pi
                for (int k = 0; k < 4; ++k) {
                    const Vec2 cc{corners[k].x - (k == 0 || k == 1 ? r : -r), corners[k].y - (k == 1 || k == 2 ? r : -r)};
                    for (int a = 0; a <= 3; ++a) {
                        const float ang = (start[k] + 0.5f * static_cast<float>(a) / 3.0f) * std::numbers::pi_v<float>;
                        loop.points.push_back({cc.x + r * std::cos(ang), y, cc.y + r * std::sin(ang)});
                    }
                }
                for (std::size_t p = 0; p < loop.points.size(); ++p)
                    loop.length += math::length(loop.points[(p + 1) % loop.points.size()] - loop.points[p]);
                c.walks.push_back(std::move(loop));
            }

            // Street furniture along the four kerbs, facing the road.
            const auto line_props = [&](Vec2 a, Vec2 d, float len, Vec2 outward) {
                const float yaw = std::atan2(outward.x, outward.y);
                if (s.light_spacing > 0.0f)
                    for (float t = 6.0f; t <= len - 6.0f; t += s.light_spacing) {
                        const Vec2 p = a + d * t - outward * 0.5f;
                        c.props.push_back({PropKind::StreetLight, {p.x, s.kerb_height, p.y}, yaw, 1.0f});
                    }
                if (s.tree_spacing > 0.0f)
                    for (float t = 6.0f + s.tree_spacing * 0.5f; t <= len - 6.0f; t += s.tree_spacing) {
                        bool near_light = false;
                        if (s.light_spacing > 0.0f) {
                            const float m = std::fmod(t - 6.0f, s.light_spacing);
                            near_light = m < 2.5f || s.light_spacing - m < 2.5f;
                        }
                        if (near_light) continue;
                        const Vec2 p = a + d * t - outward * 1.3f;
                        c.props.push_back({PropKind::Tree, {p.x, s.kerb_height, p.y}, rng.range(0.0f, 6.283f), rng.range(0.8f, 1.2f)});
                    }
            };
            const Rect& k = b.kerb;
            line_props({k.min.x, k.min.y}, {1, 0}, k.size().x, {0, -1});
            line_props({k.max.x, k.max.y}, {-1, 0}, k.size().x, {0, 1});
            line_props({k.max.x, k.min.y}, {0, 1}, k.size().y, {1, 0});
            line_props({k.min.x, k.max.y}, {0, -1}, k.size().y, {-1, 0});

            if (b.park) {
                const Rect in = inset(b.inner, 2.0f);
                for (float z = in.min.y; z <= in.max.y; z += 9.0f)
                    for (float x = in.min.x; x <= in.max.x; x += 9.0f) {
                        if (rng.unit() < 0.3f) continue;
                        const float px = std::clamp(x + rng.range(-3.0f, 3.0f), in.min.x, in.max.x);
                        const float pz = std::clamp(z + rng.range(-3.0f, 3.0f), in.min.y, in.max.y);
                        c.props.push_back({PropKind::Tree, {px, s.kerb_height, pz}, rng.range(0.0f, 6.283f), rng.range(1.0f, 1.6f)});
                    }
                continue;
            }

            // Lots: two back-to-back rows in shallow blocks, a ring around a courtyard in deep ones.
            const Rect& in = b.inner;
            const float w = in.size().x, d = in.size().y;
            std::vector<Rect> lots;
            if (std::min(w, d) <= 2.0f * s.lot_depth + 6.0f) {
                if (w >= d) {
                    const float mid = in.min.y + d * 0.5f;
                    lots_in_strip({in.min, {in.max.x, mid}}, true, s, rng, lots);
                    lots_in_strip({{in.min.x, mid}, in.max}, true, s, rng, lots);
                } else {
                    const float mid = in.min.x + w * 0.5f;
                    lots_in_strip({in.min, {mid, in.max.y}}, false, s, rng, lots);
                    lots_in_strip({{mid, in.min.y}, in.max}, false, s, rng, lots);
                }
            } else {
                const float ld = s.lot_depth;
                lots_in_strip({in.min, {in.max.x, in.min.y + ld}}, true, s, rng, lots);
                lots_in_strip({{in.min.x, in.max.y - ld}, in.max}, true, s, rng, lots);
                lots_in_strip({{in.min.x, in.min.y + ld}, {in.min.x + ld, in.max.y - ld}}, false, s, rng, lots);
                lots_in_strip({{in.max.x - ld, in.min.y + ld}, {in.max.x, in.max.y - ld}}, false, s, rng, lots);
            }
            for (const Rect& lot : lots) {
                const Rect fp{{lot.min.x + rng.range(0.3f, 1.2f), lot.min.y + rng.range(0.3f, 1.2f)},
                              {lot.max.x - rng.range(0.3f, 1.2f), lot.max.y - rng.range(0.3f, 1.2f)}};
                if (!usable(fp, 4.0f)) continue;
                const float dist = math::length(fp.center());
                const float density = std::exp(-(dist / downtown) * (dist / downtown));
                const float u = rng.range(0.25f, 1.0f);
                const float span = static_cast<float>(s.floors_max - s.floors_min);
                const auto floors = std::clamp<std::uint32_t>(
                    s.floors_min + static_cast<std::uint32_t>(std::lround(span * density * std::pow(u, 1.4f))), s.floors_min, s.floors_max);
                Building bd;
                bd.block = block_index;
                bd.lot = lot;
                bd.floors = floors;
                bd.style = floors >= 14 ? 4 + rng.below(2) : rng.below(4);
                const float height = static_cast<float>(floors) * s.floor_height;
                Rect cur = fp;
                if (floors >= 10 && rng.unit() < s.setback_chance) {
                    const std::uint32_t tiers = 2 + rng.below(2);
                    const float fr[3][3] = {{0.6f, 1.0f, 1.0f}, {0.5f, 0.78f, 1.0f}, {0.5f, 0.78f, 1.0f}};
                    for (std::uint32_t t = 0; t < tiers; ++t) {
                        if (t > 0) {
                            const float side = std::min(cur.size().x, cur.size().y);
                            const Rect next = inset(cur, side * rng.range(0.1f, 0.18f));
                            if (!usable(next, 4.0f)) break;
                            cur = next;
                        }
                        const float f = t + 1 == tiers ? 1.0f : fr[tiers - 2][t];
                        // Tiers end on a floor line.
                        const float tt = std::max(1.0f, std::round(f * static_cast<float>(floors))) * s.floor_height;
                        bd.tiers.push_back({cur, tt});
                    }
                    bd.tiers.back().top = height;
                } else {
                    bd.tiers.push_back({cur, height});
                }
                top = std::max(top, s.kerb_height + height);
                c.buildings.push_back(std::move(bd));
            }
        }
    c.bounds.expand({c.xs.front() - c.x_width.front() * 0.5f, 0.0f, c.zs.front() - c.z_width.front() * 0.5f});
    c.bounds.expand({c.xs.back() + c.x_width.back() * 0.5f, top, c.zs.back() + c.z_width.back() * 0.5f});
    return c;
}

} // namespace victus::procgen
