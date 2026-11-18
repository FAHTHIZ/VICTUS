// SPDX-License-Identifier: Apache-2.0
// Convex decomposition of simple 2D polygons for Box2D (whose polygons are convex with at most
// B2_MAX_POLYGON_VERTICES = 8 points): ear clipping into triangles, then Hertel-Mehlhorn merging
// of neighbouring pieces across diagonals while the union stays convex and small enough. Hertel-
// Mehlhorn yields at most 4x the optimal number of convex pieces; it is O(n^2) here, bounded by
// kMaxPolygonPoints.
#include "victus/physics/physics.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace victus::physics {

namespace {

float cross(math::Vec2 a, math::Vec2 b) noexcept { return a.x * b.y - a.y * b.x; }
float cross3(math::Vec2 o, math::Vec2 a, math::Vec2 b) noexcept { return cross(a - o, b - o); }

int orientation(math::Vec2 a, math::Vec2 b, math::Vec2 c, float eps) noexcept {
    const float v = cross3(a, b, c);
    return v > eps ? 1 : v < -eps ? -1 : 0;
}

bool on_segment(math::Vec2 a, math::Vec2 b, math::Vec2 p) noexcept {
    return std::min(a.x, b.x) <= p.x && p.x <= std::max(a.x, b.x) && std::min(a.y, b.y) <= p.y && p.y <= std::max(a.y, b.y);
}

bool segments_touch(math::Vec2 a, math::Vec2 b, math::Vec2 c, math::Vec2 d, float eps) noexcept {
    const int o1 = orientation(a, b, c, eps), o2 = orientation(a, b, d, eps);
    const int o3 = orientation(c, d, a, eps), o4 = orientation(c, d, b, eps);
    if (o1 != o2 && o3 != o4 && o1 * o2 <= 0 && o3 * o4 <= 0 && (o1 || o2 || o3 || o4)) return true;
    return (o1 == 0 && on_segment(a, b, c)) || (o2 == 0 && on_segment(a, b, d)) || (o3 == 0 && on_segment(c, d, a)) ||
           (o4 == 0 && on_segment(c, d, b));
}

// Area tolerance relative to the outline's size, so millimetre and kilometre outlines behave alike.
float area_eps(const std::vector<math::Vec2>& p) noexcept {
    math::Vec2 lo = p.empty() ? math::Vec2{} : p[0], hi = lo;
    for (const math::Vec2 q : p) {
        lo = {std::min(lo.x, q.x), std::min(lo.y, q.y)};
        hi = {std::max(hi.x, q.x), std::max(hi.y, q.y)};
    }
    const float l = std::max({hi.x - lo.x, hi.y - lo.y, 1e-6f});
    return 1e-6f * l * l;
}

// Drops repeated points (including a closing copy of the first) and points on a straight line
// between their neighbours.
std::vector<math::Vec2> clean(std::vector<math::Vec2> p, float eps) {
    bool changed = true;
    while (changed && p.size() >= 3) {
        changed = false;
        for (std::size_t i = 0; i < p.size() && p.size() >= 3; ++i) {
            const math::Vec2 a = p[(i + p.size() - 1) % p.size()], b = p[i], c = p[(i + 1) % p.size()];
            const bool repeated = std::fabs(b.x - c.x) <= 1e-7f && std::fabs(b.y - c.y) <= 1e-7f;
            if (repeated || std::fabs(cross3(a, b, c)) <= eps) {
                p.erase(p.begin() + static_cast<std::ptrdiff_t>(i));
                changed = true;
                --i;
            }
        }
    }
    return p;
}

bool convex(const std::vector<std::uint32_t>& loop, const std::vector<math::Vec2>& p, float eps) noexcept {
    const std::size_t n = loop.size();
    for (std::size_t i = 0; i < n; ++i)
        if (cross3(p[loop[i]], p[loop[(i + 1) % n]], p[loop[(i + 2) % n]]) < -eps) return false;
    return true;
}

} // namespace

float signed_area2(const std::vector<math::Vec2>& p) noexcept {
    float a = 0;
    for (std::size_t i = 0; i < p.size(); ++i) a += cross(p[i], p[(i + 1) % p.size()]);
    return a;
}

bool self_intersects(const std::vector<math::Vec2>& p, bool closed) noexcept {
    const std::size_t n = p.size();
    const std::size_t edges = closed ? n : (n == 0 ? 0 : n - 1);
    if (edges < 3) return false;
    const float eps = area_eps(p) * 1e-3f;
    for (std::size_t i = 0; i < edges; ++i) {
        const math::Vec2 a = p[i], b = p[(i + 1) % n];
        for (std::size_t j = i + 2; j < edges; ++j) {
            if (closed && i == 0 && j == edges - 1) continue; // adjacent through the closing edge
            if (segments_touch(a, b, p[j], p[(j + 1) % n], eps)) return true;
        }
    }
    return false;
}

Result<std::vector<std::vector<math::Vec2>>> decompose_polygon(const std::vector<math::Vec2>& input, std::size_t max_vertices) {
    if (max_vertices < 3) return make_error(ErrorCode::InvalidArgument, "polygon: max_vertices must be at least 3");
    if (input.size() > kMaxPolygonPoints)
        return make_error(ErrorCode::InvalidArgument,
                          std::format("polygon: {} points (at most {})", input.size(), kMaxPolygonPoints));
    for (const math::Vec2 q : input)
        if (!std::isfinite(q.x) || !std::isfinite(q.y)) return make_error(ErrorCode::InvalidArgument, "polygon: non-finite point");
    const float eps = area_eps(input);
    std::vector<math::Vec2> p = clean(input, eps);
    if (p.size() < 3) return make_error(ErrorCode::InvalidArgument, "polygon: needs at least 3 distinct, non-collinear points");
    if (self_intersects(p, true)) return make_error(ErrorCode::InvalidArgument, "polygon: edges cross each other");
    const float area = signed_area2(p);
    if (std::fabs(area) <= eps) return make_error(ErrorCode::InvalidArgument, "polygon: zero area");
    if (area < 0) std::reverse(p.begin(), p.end());

    // Ear clipping (counter-clockwise): a convex corner whose triangle contains no other point.
    std::vector<std::vector<std::uint32_t>> pieces;
    std::vector<std::uint32_t> ring(p.size());
    for (std::uint32_t i = 0; i < ring.size(); ++i) ring[i] = i;
    while (ring.size() > 3) {
        const std::size_t m = ring.size();
        bool clipped = false;
        for (std::size_t i = 0; i < m && !clipped; ++i) {
            const std::uint32_t a = ring[(i + m - 1) % m], b = ring[i], c = ring[(i + 1) % m];
            if (cross3(p[a], p[b], p[c]) <= eps) continue; // reflex or flat
            bool empty = true;
            for (const std::uint32_t k : ring) {
                if (k == a || k == b || k == c) continue;
                if (cross3(p[a], p[b], p[k]) >= -eps && cross3(p[b], p[c], p[k]) >= -eps && cross3(p[c], p[a], p[k]) >= -eps) {
                    empty = false;
                    break;
                }
            }
            if (!empty) continue;
            pieces.push_back({a, b, c});
            ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(i));
            clipped = true;
        }
        if (!clipped) return make_error(ErrorCode::InvalidArgument, "polygon: cannot be triangulated (degenerate outline)");
    }
    if (cross3(p[ring[0]], p[ring[1]], p[ring[2]]) > eps) pieces.push_back(ring);

    // Hertel-Mehlhorn: remove diagonals whose removal keeps both sides one convex piece.
    for (bool merged = true; merged;) {
        merged = false;
        for (std::size_t i = 0; i < pieces.size() && !merged; ++i)
            for (std::size_t j = i + 1; j < pieces.size() && !merged; ++j) {
                const auto& pi = pieces[i];
                const auto& pj = pieces[j];
                if (pi.size() + pj.size() - 2 > max_vertices) continue;
                for (std::size_t k = 0; k < pi.size() && !merged; ++k) {
                    const std::uint32_t u = pi[k], v = pi[(k + 1) % pi.size()];
                    for (std::size_t l = 0; l < pj.size(); ++l) {
                        if (pj[l] != v || pj[(l + 1) % pj.size()] != u) continue;
                        std::vector<std::uint32_t> joined;
                        joined.reserve(pi.size() + pj.size() - 2);
                        for (std::size_t t = 1; t <= pi.size(); ++t) joined.push_back(pi[(k + t) % pi.size()]); // v .. u
                        for (std::size_t t = 2; t < pj.size(); ++t) joined.push_back(pj[(l + t) % pj.size()]);   // after u .. before v
                        if (convex(joined, p, eps)) {
                            pieces[i] = std::move(joined);
                            pieces.erase(pieces.begin() + static_cast<std::ptrdiff_t>(j));
                            merged = true;
                        }
                        break;
                    }
                }
            }
    }

    std::vector<std::vector<math::Vec2>> out;
    out.reserve(pieces.size());
    for (const auto& loop : pieces) {
        std::vector<math::Vec2> poly;
        poly.reserve(loop.size());
        for (const std::uint32_t k : loop) poly.push_back(p[k]);
        out.push_back(std::move(poly));
    }
    return out;
}

} // namespace victus::physics
