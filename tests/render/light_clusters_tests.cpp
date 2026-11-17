// SPDX-License-Identifier: Apache-2.0
// Clustered light assignment is conservative (every light that reaches a point is in that
// point's cluster) while still culling most lights from most clusters.
#include "doctest.h"
#include "victus/render/light_clusters.hpp"

#include <cmath>
#include <random>

using namespace victus;
using namespace victus::render;
using math::Vec3;
using math::Vec4;

namespace {
GpuLight point(Vec3 p, float range) {
    GpuLight l{};
    l.position_range = Vec4(p, range);
    l.color_intensity = {1, 1, 1, 1};
    return l;
}
GpuLight spot(Vec3 p, Vec3 dir, float range, float outer) {
    GpuLight l = point(p, range);
    l.direction_cos_outer = Vec4(math::normalize(dir), std::cos(outer));
    l.spot = {std::cos(outer * 0.5f), 1, 0, 0};
    return l;
}
bool reaches(const GpuLight& l, Vec3 p) {
    const Vec3 lp{l.position_range.x, l.position_range.y, l.position_range.z};
    const Vec3 d = p - lp;
    const float dist = math::length(d);
    if (dist > l.position_range.w) return false;
    if (l.spot.y < 0.5f) return true;
    const Vec3 axis{l.direction_cos_outer.x, l.direction_cos_outer.y, l.direction_cos_outer.z};
    return dist < 1e-4f || math::dot(d / dist, axis) >= l.direction_cos_outer.w;
}
} // namespace

TEST_CASE("light clusters: assignment is conservative and culls") {
    const math::Mat4 view = math::Mat4::look_at({0, 3, 10}, {0, 0, 0}, {0, 1, 0});
    const float aspect = 16.0f / 9.0f;
    const math::Mat4 proj = math::Mat4::perspective(1.0f, aspect, 0.1f, 200.0f);
    const math::Mat4 inv_view = math::inverse(view);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    std::vector<GpuLight> lights;
    for (int i = 0; i < 300; ++i) {
        const Vec3 p{u(rng) * 30.0f, u(rng) * 3.0f + 1.0f, u(rng) * 30.0f - 10.0f};
        if (i % 3 == 0) lights.push_back(spot(p, {u(rng), -1.0f, u(rng)}, 2.0f + 6.0f * (u(rng) + 1.0f), 0.3f + 0.6f * (u(rng) + 1.0f)));
        else lights.push_back(point(p, 1.0f + 3.0f * (u(rng) + 1.0f)));
    }
    const LightClusters c = build_light_clusters(lights, view, proj, 0.1f, 200.0f);
    CHECK(c.cluster_count() == 16 * 9 * 24);
    CHECK(c.overflowed_clusters == 0);
    // Random points inside the frustum: every light that reaches one is listed in its cluster.
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    int checked = 0, missing = 0;
    for (int i = 0; i < 20000; ++i) {
        const float sx = u01(rng), sy = u01(rng);
        const float depth = 0.1f * std::pow(400.0f, u01(rng)); // 0.1..40, log-distributed
        const Vec3 vp{(sx * 2 - 1) * depth / proj[0].x, (1 - sy * 2) * depth / proj[1].y, -depth};
        const Vec3 wp = inv_view.transform_point(vp);
        const auto list = c.lights_in(c.cluster_of({sx, sy}, depth));
        for (std::uint32_t li = 0; li < lights.size(); ++li) {
            if (!reaches(lights[li], wp)) continue;
            ++checked;
            if (std::find(list.begin(), list.end(), li) == list.end()) ++missing;
        }
    }
    MESSAGE("light/point pairs checked " << checked << ", indices " << c.index_count << ", max per cluster " << c.max_per_cluster);
    CHECK(checked > 1000);
    CHECK(missing == 0);
    // Culling: far fewer entries than every light in every cluster, and no cluster sees them all.
    CHECK(c.index_count < lights.size() * c.cluster_count() / 25); // ~2% of the full light x cluster table
    CHECK(c.max_per_cluster < 100);
}

TEST_CASE("light clusters: lights behind the camera or beyond far are not assigned") {
    const math::Mat4 view = math::Mat4::look_at({0, 0, 0}, {0, 0, -1}, {0, 1, 0});
    const math::Mat4 proj = math::Mat4::perspective(1.0f, 1.0f, 0.1f, 50.0f);
    const std::vector<GpuLight> lights{point({0, 0, 5}, 2.0f), point({0, 0, -80}, 5.0f), point({0, 0, -5}, 1.0f)};
    const LightClusters c = build_light_clusters(lights, view, proj, 0.1f, 50.0f);
    for (std::uint32_t i = 0; i < c.cluster_count(); ++i)
        for (std::uint32_t li : c.lights_in(i)) CHECK(li == 2);
    const auto centre = c.lights_in(c.cluster_of({0.5f, 0.5f}, 5.0f));
    REQUIRE(centre.size() == 1);
    CHECK(centre[0] == 2);
    CHECK(c.lights_in(c.cluster_of({0.05f, 0.05f}, 5.0f)).empty()); // screen corner, outside the sphere
}

TEST_CASE("light clusters: spot bounding sphere encloses the cone and caps overflow") {
    for (float outer : {0.2f, 0.7f, 1.2f, 1.5f}) {
        const GpuLight s = spot({1, 2, 3}, {0, -1, 0.2f}, 6.0f, outer);
        const Vec4 b = light_bounding_sphere(s);
        const Vec3 c{b.x, b.y, b.z};
        const Vec3 axis = math::normalize(Vec3{0, -1, 0.2f});
        const Vec3 side = math::normalize(math::cross(axis, {1, 0, 0}));
        CHECK(math::length(Vec3{1, 2, 3} - c) <= b.w + 1e-3f);
        for (float t : {0.25f, 0.5f, 1.0f}) { // rim points of the spherical cap
            const Vec3 rim = Vec3{1, 2, 3} + (axis * std::cos(outer) + side * std::sin(outer)) * (6.0f * t);
            CHECK(math::length(rim - c) <= b.w + 1e-3f);
        }
        if (outer < 0.78f) CHECK(b.w < 6.0f); // narrow cones are tighter than the range sphere
    }
    std::vector<GpuLight> crowd(40, point({0, 0, -5}, 3.0f));
    ClusterGridSettings g;
    g.max_lights_per_cluster = 8;
    const math::Mat4 view = math::Mat4::identity();
    const LightClusters c = build_light_clusters(crowd, view, math::Mat4::perspective(1.0f, 1.0f, 0.1f, 50.0f), 0.1f, 50.0f, g);
    CHECK(c.max_per_cluster == 8);
    CHECK(c.overflowed_clusters > 0);
    CHECK(c.lights_in(c.cluster_of({0.5f, 0.5f}, 5.0f)).size() == 8);
}
