// SPDX-License-Identifier: Apache-2.0
// Planar water reflections (ADR-0260): the mirrored view with its oblique near plane, the quality
// presets and the rendering-path report.
#include "doctest.h"
#include "victus/render/render_path.hpp"
#include "victus/render/scene_renderer.hpp"

#include <cmath>

using namespace victus;
using namespace victus::render;
using math::Vec3;
using math::Vec4;

namespace {
Vec4 clip(const math::Mat4& vp, Vec3 p) { return vp * Vec4(p, 1.0f); }
} // namespace

TEST_CASE("planar reflection view: mirrors about the plane, clips below it, keeps winding") {
    const Camera3D cam = Camera3D::look_at({1, 3, 6}, {0, 0.5f, -4});
    const float aspect = 16.0f / 9.0f, h = 0.5f;
    const PlanarReflectionView v = planar_reflection_view(cam, aspect, h);
    const math::Mat4 main_vp = cam.projection(aspect) * cam.view;
    CHECK(v.height == doctest::Approx(h));
    CHECK(v.position.y == doctest::Approx(2 * h - 3.0f));
    CHECK(v.position.x == doctest::Approx(1.0f));
    CHECK(v.forward.y == doctest::Approx(-cam.forward().y));

    // A point above the water lands where its mirror image appears in the main view, with y negated.
    for (const Vec3 p : {Vec3{0, 2.0f, -3}, Vec3{-2, 1.0f, -8}, Vec3{1.5f, 4.0f, -1}}) {
        const Vec4 a = clip(v.view_proj, p);
        const Vec4 b = clip(main_vp, {p.x, 2 * h - p.y, p.z});
        REQUIRE(a.w > 0.0f);
        CHECK(a.x / a.w == doctest::Approx(b.x / b.w).epsilon(1e-3));
        CHECK(a.y / a.w == doctest::Approx(-b.y / b.w).epsilon(1e-3));
        const float z = a.z / a.w;
        CHECK(z > 0.0f);
        CHECK(z <= 1.0f);
        // The sky matrix agrees on screen position.
        const Vec4 s = clip(v.sky_view_proj, p);
        CHECK(s.x / s.w == doctest::Approx(a.x / a.w).epsilon(1e-3));
        CHECK(s.y / s.w == doctest::Approx(a.y / a.w).epsilon(1e-3));
    }
    // Points on the plane sit on the near plane; points under the water are clipped (z < 0).
    for (const Vec3 p : {Vec3{0, h, -3}, Vec3{2, h, -10}}) {
        const Vec4 a = clip(v.view_proj, p);
        CHECK(a.z / a.w == doctest::Approx(0.0f).epsilon(1e-3).scale(1.0));
    }
    for (const Vec3 p : {Vec3{0, h - 0.2f, -3}, Vec3{-1, h - 2.0f, -6}}) {
        const Vec4 a = clip(v.view_proj, p);
        REQUIRE(a.w > 0.0f);
        CHECK(a.z / a.w < 0.0f);
    }
    // Winding: a triangle that faces the camera in the main view keeps its screen orientation in
    // the reflection (the mirror and the clip-y flip cancel), so back-face culling stays valid.
    const auto signed_area = [](const math::Mat4& vp, Vec3 a, Vec3 b, Vec3 c) {
        const Vec4 pa = vp * Vec4(a, 1), pb = vp * Vec4(b, 1), pc = vp * Vec4(c, 1);
        const float ax = pa.x / pa.w, ay = pa.y / pa.w, bx = pb.x / pb.w, by = pb.y / pb.w, cx = pc.x / pc.w, cy = pc.y / pc.w;
        return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    };
    // A wall facing +z (towards the camera) stays front-facing in the reflection.
    const Vec3 t0{-1, 1, -3}, t1{1, 1, -3}, t2{0, 2, -3};
    CHECK(signed_area(main_vp, t0, t1, t2) > 0.0f);
    CHECK(signed_area(v.view_proj, t0, t1, t2) > 0.0f);
}

TEST_CASE("oblique near plane: the plane maps to depth 0 and the far corner stays at depth 1") {
    const math::Mat4 proj = math::Mat4::perspective(math::radians(60.0f), 1.5f, 0.1f, 100.0f);
    // A tilted view-space plane in front of the camera (camera on its negative side).
    const Vec3 n = math::normalize(Vec3{0.2f, 0.5f, -1.0f});
    const float dist = 2.0f;
    const Vec4 plane{n.x, n.y, n.z, -dist};
    REQUIRE(plane.w < 0.0f);
    const math::Mat4 m = oblique_near_plane(proj, plane);
    const Vec3 on_plane = n * dist;
    const Vec4 c = m * Vec4(on_plane, 1.0f);
    CHECK(c.z / c.w == doctest::Approx(0.0f).scale(1.0));
    const Vec4 beyond = m * Vec4(n * (dist + 3.0f), 1.0f);
    CHECK(beyond.z / beyond.w > 0.0f);
    // x, y and w are untouched.
    for (int i = 0; i < 4; ++i) {
        CHECK(m.c[i].x == proj.c[i].x);
        CHECK(m.c[i].y == proj.c[i].y);
        CHECK(m.c[i].w == proj.c[i].w);
    }
    // A degenerate plane leaves the projection alone.
    CHECK(oblique_near_plane(proj, {0, 0, 0, 0}) == proj);
}

TEST_CASE("planar reflections: presets, sanitising and the rendering-path report") {
    CHECK_FALSE(RenderSettings::preset(QualityTier::Low).planar_reflections);
    CHECK_FALSE(RenderSettings::preset(QualityTier::Medium).planar_reflections);
    CHECK(RenderSettings::preset(QualityTier::High).planar_reflections);
    CHECK(RenderSettings::preset(QualityTier::High).planar_reflection_scale == doctest::Approx(0.5f));
    CHECK(RenderSettings::preset(QualityTier::Ultra).planar_reflection_scale == doctest::Approx(1.0f));
    RenderSettings s;
    s.planar_reflection_scale = 7.0f;
    CHECK(s.sanitized().planar_reflection_scale == doctest::Approx(1.0f));
    s.planar_reflection_scale = 0.0f;
    CHECK(s.sanitized().planar_reflection_scale == doctest::Approx(0.25f));
    s.planar_reflection_scale = std::nanf("");
    CHECK(s.sanitized().planar_reflection_scale == doctest::Approx(0.5f));

    RenderPathFacts f;
    f.render_width = f.output_width = 1280;
    f.render_height = f.output_height = 720;
    const auto feature = [](const RenderPathReport& r) {
        const PathFeature* p = r.find("planar_reflections");
        REQUIRE(p != nullptr);
        return *p;
    };
    const RenderSettings high = RenderSettings::preset(QualityTier::High);
    CHECK(feature(describe_render_path(high, f)).reason == PathReason::NotNeeded); // no water
    f.water_submitted = f.water_in_view = f.water_drawn = 1;
    f.water_budget = 16;
    CHECK(feature(describe_render_path(high, f)).reason == PathReason::NotNeeded); // camera below
    f.planar = true;
    f.planar_width = 640;
    f.planar_height = 360;
    f.planar_draws = 12;
    f.planar_plane = 0.5f;
    PathFeature p = feature(describe_render_path(high, f));
    CHECK(p.state == PathState::Reduced); // half of Ultra's resolution
    CHECK(p.reason == PathReason::QualityTier);
    CHECK(p.detail == "640x360, 12 draws mirrored about y = 0.5");
    p = feature(describe_render_path(RenderSettings::preset(QualityTier::Ultra), f));
    CHECK(p.state == PathState::Active);
    RenderSettings tweaked = high;
    tweaked.planar_reflection_scale = 0.25f;
    CHECK(feature(describe_render_path(tweaked, f)).reason == PathReason::Setting);
    CHECK(feature(describe_render_path(RenderSettings::preset(QualityTier::Medium), f)).reason == PathReason::QualityTier);
    RenderSettings off = high;
    off.planar_reflections = false;
    CHECK(feature(describe_render_path(off, f)).reason == PathReason::Setting);
    f.capture_mode = true;
    CHECK(feature(describe_render_path(high, f)).reason == PathReason::CaptureMode);
}
