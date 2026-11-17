// SPDX-License-Identifier: Apache-2.0
// Transform gizmo math: projection, hit testing, drag solving and snapping (no GPU needed).
#include "victus/editor/gizmo.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/render/sprite_batch.hpp"

#include "doctest.h"

#include <cmath>

using namespace victus;
using namespace victus::editor;
using math::Vec2;
using math::Vec3;

namespace {
constexpr Vec2 kSize{800, 600};

gizmo::View perspective(Vec3 eye, Vec3 target = {}) {
    const render::Camera3D cam = render::Camera3D::look_at(eye, target);
    return gizmo::View(cam.projection(kSize.x / kSize.y) * cam.view, kSize);
}
float handle_length(const gizmo::View& v, Vec3 p) { return v.world_per_pixel(p) * gizmo::Options{}.size_pixels; }
Vec2 px(const gizmo::View& v, Vec3 p) {
    const auto s = v.project(p);
    REQUIRE(s.has_value());
    return *s;
}
void check_vec(Vec3 a, Vec3 b, float eps = 1e-2f) {
    CHECK(a.x == doctest::Approx(b.x).epsilon(eps).scale(1));
    CHECK(a.y == doctest::Approx(b.y).epsilon(eps).scale(1));
    CHECK(a.z == doctest::Approx(b.z).epsilon(eps).scale(1));
}
} // namespace

TEST_CASE("gizmo: view projection, rays and pixel size") {
    const gizmo::View v = perspective({0, 0, 10});
    CHECK_FALSE(v.orthographic());
    const Vec2 c = px(v, {0, 0, 0});
    CHECK(c.x == doctest::Approx(400));
    CHECK(c.y == doctest::Approx(300));
    check_vec(v.ray(c).direction, {0, 0, -1}, 1e-4f);
    // +Y is up on screen, +X is right.
    CHECK(px(v, {0, 1, 0}).y < 300);
    CHECK(px(v, {1, 0, 0}).x > 400);
    // One pixel at distance 10 with a 60 degree vertical field of view.
    CHECK(v.world_per_pixel({0, 0, 0}) == doctest::Approx(2.0f * 10.0f * std::tan(math::radians(30.0f)) / 600.0f).epsilon(1e-3));
    CHECK_FALSE(v.project({0, 0, 20}).has_value()); // behind the camera
    // The ray through a projected point passes through it.
    const Vec3 p{1.5f, -0.7f, 2.0f};
    const auto r = v.ray(px(v, p));
    const Vec3 to = p - r.origin;
    CHECK(math::length(to - r.direction * math::dot(to, r.direction)) < 1e-3f);

    // Orthographic 2D camera.
    render::Camera2D cam2{{3, 1}, 1.0f, 0.0f, 50.0f};
    const gizmo::View o(cam2.view_projection(kSize.x, kSize.y), kSize);
    CHECK(o.orthographic());
    const Vec2 oc = px(o, {3, 1, 0});
    CHECK(oc.x == doctest::Approx(400));
    CHECK(oc.y == doctest::Approx(300));
    CHECK(o.world_per_pixel({0, 0, 0}) == doctest::Approx(1.0f / 50.0f).epsilon(1e-3));
    CHECK(o.world_per_pixel({10, 5, 0}) == doctest::Approx(1.0f / 50.0f).epsilon(1e-3));
    check_vec(o.ray({100, 100}).direction, {0, 0, -1}, 1e-4f);
}

TEST_CASE("gizmo: translate handles are hit where they are drawn") {
    const gizmo::View v = perspective({6, 5, 8});
    const gizmo::Frame f{{0, 0, 0}, {}};
    const float L = handle_length(v, f.pivot);
    CHECK(gizmo::hit_test(gizmo::Mode::Translate, v, f, px(v, {L * 0.7f, 0, 0})) == gizmo::Handle::X);
    CHECK(gizmo::hit_test(gizmo::Mode::Translate, v, f, px(v, {0, L * 0.7f, 0})) == gizmo::Handle::Y);
    CHECK(gizmo::hit_test(gizmo::Mode::Translate, v, f, px(v, {0, 0, L * 0.7f})) == gizmo::Handle::Z);
    CHECK(gizmo::hit_test(gizmo::Mode::Translate, v, f, px(v, {0, 0, 0})) == gizmo::Handle::View);
    CHECK(gizmo::hit_test(gizmo::Mode::Translate, v, f, px(v, {L * 0.34f, L * 0.34f, 0})) == gizmo::Handle::XY);
    CHECK(gizmo::hit_test(gizmo::Mode::Translate, v, f, px(v, {0, L * 0.34f, L * 0.34f})) == gizmo::Handle::YZ);
    CHECK(gizmo::hit_test(gizmo::Mode::Translate, v, f, px(v, {L * 0.34f, 0, L * 0.34f})) == gizmo::Handle::ZX);
    CHECK(gizmo::hit_test(gizmo::Mode::Translate, v, f, px(v, {L * 3, L * 3, 0})) == gizmo::Handle::None);
    // Local space follows the frame orientation: the X handle now lies along world Z.
    const gizmo::Frame turned{{0, 0, 0}, math::Quat::axis_angle({0, 1, 0}, math::radians(-90.0f))};
    CHECK(gizmo::hit_test(gizmo::Mode::Translate, v, turned, px(v, {0, 0, L * 0.7f})) == gizmo::Handle::X);
    // An axis pointing at the camera is hidden and cannot be grabbed.
    const gizmo::View front = perspective({0, 0, 10});
    CHECK(gizmo::hit_test(gizmo::Mode::Translate, front, f, px(front, {0, 0, 0})) == gizmo::Handle::View);
    CHECK_FALSE(gizmo::Drag::begin(gizmo::Mode::Translate, gizmo::Handle::Z, front, f, px(front, {0, 0, 0})).has_value());
    // Handles keep their size on screen at any distance.
    const gizmo::View far = perspective({60, 50, 80});
    const float Lf = handle_length(far, f.pivot);
    CHECK(Lf > L * 5);
    CHECK(math::length(px(far, {Lf, 0, 0}) - px(far, {0, 0, 0})) ==
          doctest::Approx(math::length(px(v, {L, 0, 0}) - px(v, {0, 0, 0}))).epsilon(0.15));
}

TEST_CASE("gizmo: axis, plane and view drags follow the pointer and snap") {
    const gizmo::View v = perspective({6, 5, 8});
    const gizmo::Frame f{{1, 0, -1}, {}};
    const float L = handle_length(v, f.pivot);
    const Vec3 grab = f.pivot + Vec3{L * 0.7f, 0, 0};
    auto d = gizmo::Drag::begin(gizmo::Mode::Translate, gizmo::Handle::X, v, f, px(v, grab));
    REQUIRE(d.has_value());
    check_vec(d->update(v, px(v, grab + Vec3{2, 0, 0}), nullptr).translation, {2, 0, 0});
    check_vec(d->update(v, px(v, grab + Vec3{-1.25f, 0, 0}), nullptr).translation, {-1.25f, 0, 0});
    // Off-axis pointer motion only moves along the axis.
    CHECK(d->update(v, px(v, grab + Vec3{1, 1, 0}), nullptr).translation.y == doctest::Approx(0).scale(1));
    gizmo::Snap snap;
    snap.translate = 0.5f;
    check_vec(d->update(v, px(v, grab + Vec3{1.3f, 0, 0}), &snap).translation, {1.5f, 0, 0});
    check_vec(d->update(v, px(v, grab + Vec3{0.2f, 0, 0}), &snap).translation, {0, 0, 0});

    // ZX plane (the ground) from above.
    const Vec3 pgrab = f.pivot + Vec3{L * 0.34f, 0, L * 0.34f};
    auto p = gizmo::Drag::begin(gizmo::Mode::Translate, gizmo::Handle::ZX, v, f, px(v, pgrab));
    REQUIRE(p.has_value());
    check_vec(p->update(v, px(v, pgrab + Vec3{1, 0, 2}), nullptr).translation, {1, 0, 2});
    check_vec(p->update(v, px(v, pgrab + Vec3{0.9f, 0, -0.3f}), &snap).translation, {1, 0, -0.5f});

    // View-plane move keeps the depth.
    auto c = gizmo::Drag::begin(gizmo::Mode::Translate, gizmo::Handle::View, v, f, px(v, f.pivot));
    REQUIRE(c.has_value());
    const Vec3 t = c->update(v, px(v, f.pivot) + Vec2{40, -25}, nullptr).translation;
    CHECK(math::length(t) > 0.1f);
    CHECK(std::fabs(math::dot(t, v.view_direction(f.pivot))) < 1e-3f);
    const Vec2 moved = px(v, f.pivot + t);
    CHECK(moved.x == doctest::Approx(px(v, f.pivot).x + 40).epsilon(0.01));
}

TEST_CASE("gizmo: rotate rings, unwrapping, snapping and the view ring") {
    const gizmo::View v = perspective({0.01f, 10, 0.02f}); // looking down -Y
    const gizmo::Frame f{{0, 0, 0}, {}};
    const float L = handle_length(v, f.pivot);
    const Vec3 grab{L, 0, 0};
    CHECK(gizmo::hit_test(gizmo::Mode::Rotate, v, f, px(v, grab)) == gizmo::Handle::Y);
    auto d = gizmo::Drag::begin(gizmo::Mode::Rotate, gizmo::Handle::Y, v, f, px(v, grab));
    REQUIRE(d.has_value());
    // Positive rotation about +Y turns +X towards -Z.
    gizmo::Delta r = d->update(v, px(v, {0, 0, -L}), nullptr);
    CHECK(r.angle_degrees == doctest::Approx(90).epsilon(0.02));
    check_vec(r.rotation.rotate({1, 0, 0}), {0, 0, -1}, 0.03f);
    // Keep going around: the angle unwraps past 180 instead of jumping.
    for (int deg = 120; deg <= 350; deg += 30) {
        const float a = math::radians(static_cast<float>(deg));
        r = d->update(v, px(v, {L * std::cos(a), 0, -L * std::sin(a)}), nullptr);
    }
    CHECK(r.angle_degrees == doctest::Approx(330).epsilon(0.02));
    gizmo::Snap snap;
    snap.rotate_degrees = 15.0f;
    const float a50 = math::radians(50.0f);
    // Past a full turn the angle keeps counting (330 + 80 = 410, snapped to 405).
    r = d->update(v, px(v, {L * std::cos(a50), 0, -L * std::sin(a50)}), &snap);
    CHECK(r.angle_degrees == doctest::Approx(405.0f));
    auto fresh = gizmo::Drag::begin(gizmo::Mode::Rotate, gizmo::Handle::Y, v, f, px(v, grab));
    REQUIRE(fresh.has_value());
    CHECK(fresh->update(v, px(v, {L * std::cos(a50), 0, -L * std::sin(a50)}), &snap).angle_degrees == doctest::Approx(45.0f));

    // The outer ring turns about the view direction: on screen a quarter turn counter-clockwise.
    const Vec2 c = px(v, f.pivot);
    const float R = gizmo::Options{}.size_pixels * 1.18f;
    CHECK(gizmo::hit_test(gizmo::Mode::Rotate, v, f, c + Vec2{R, 0}) == gizmo::Handle::View);
    auto vr = gizmo::Drag::begin(gizmo::Mode::Rotate, gizmo::Handle::View, v, f, c + Vec2{R, 0});
    REQUIRE(vr.has_value());
    r = vr->update(v, c + Vec2{0, -R}, nullptr);
    CHECK(std::fabs(r.angle_degrees) == doctest::Approx(90).epsilon(0.03));

    // A nearly edge-on ring drags along its screen tangent instead of a degenerate plane.
    const gizmo::View side = perspective({0, 1.5f, 10});
    const float Ls = handle_length(side, f.pivot);
    const auto s0 = gizmo::hit_test(gizmo::Mode::Rotate, side, f, px(side, {0, 0, Ls}));
    CHECK(s0 == gizmo::Handle::Y);
    auto e = gizmo::Drag::begin(gizmo::Mode::Rotate, gizmo::Handle::Y, side, f, px(side, {Ls, 0, 0}));
    REQUIRE(e.has_value());
    const gizmo::Delta er = e->update(side, px(side, {Ls, 0, 0}) + Vec2{0, 0}, nullptr);
    CHECK(er.angle_degrees == doctest::Approx(0).scale(1));
    CHECK(std::fabs(e->update(side, px(side, {Ls, 0, 0}) + Vec2{0, -40}, nullptr).angle_degrees) > 5.0f);
}

TEST_CASE("gizmo: scale axes and uniform scale") {
    const gizmo::View v = perspective({6, 5, 8});
    const gizmo::Frame f{{0, 0, 0}, {}};
    const float L = handle_length(v, f.pivot);
    CHECK(gizmo::hit_test(gizmo::Mode::Scale, v, f, px(v, {L * 0.95f, 0, 0})) == gizmo::Handle::X);
    auto d = gizmo::Drag::begin(gizmo::Mode::Scale, gizmo::Handle::X, v, f, px(v, {L * 0.8f, 0, 0}));
    REQUIRE(d.has_value());
    gizmo::Delta s = d->update(v, px(v, {L * 1.6f, 0, 0}), nullptr);
    CHECK(s.scale.x == doctest::Approx(2.0f).epsilon(0.02));
    CHECK(s.scale.y == 1.0f);
    CHECK(s.scale.z == 1.0f);
    gizmo::Snap snap;
    snap.scale = 0.25f;
    CHECK(d->update(v, px(v, {L * 1.2f, 0, 0}), &snap).scale.x == doctest::Approx(1.5f));
    // Dragging through the pivot never flips or zeroes the scale.
    CHECK(d->update(v, px(v, {-L, 0, 0}), nullptr).scale.x > 0.0f);

    auto u = gizmo::Drag::begin(gizmo::Mode::Scale, gizmo::Handle::View, v, f, px(v, f.pivot));
    REQUIRE(u.has_value());
    s = u->update(v, px(v, f.pivot) + Vec2{48, 0}, nullptr);
    CHECK(s.scale.x == doctest::Approx(1.5f));
    CHECK(s.scale.y == doctest::Approx(1.5f));
    CHECK(s.scale.z == doctest::Approx(1.5f));
    CHECK(gizmo::describe(gizmo::Mode::Scale, s) == "Scale 1.500, 1.500, 1.500");
}

TEST_CASE("gizmo: 2D views show only the handles that make sense") {
    render::Camera2D cam2{{0, 0}, 1.0f, 0.0f, 40.0f};
    const gizmo::View o(cam2.view_projection(kSize.x, kSize.y), kSize);
    const gizmo::Frame f{{2, 1, 0}, {}};
    const float L = handle_length(o, f.pivot);
    CHECK(gizmo::hit_test(gizmo::Mode::Translate, o, f, px(o, f.pivot + Vec3{L * 0.7f, 0, 0})) == gizmo::Handle::X);
    CHECK(gizmo::hit_test(gizmo::Mode::Translate, o, f, px(o, f.pivot + Vec3{L * 0.34f, L * 0.34f, 0})) == gizmo::Handle::XY);
    // The ring about Z is the only rotation ring; X and Y rings are edge-on.
    CHECK(gizmo::hit_test(gizmo::Mode::Rotate, o, f, px(o, f.pivot + Vec3{L, 0, 0})) == gizmo::Handle::Z);
    CHECK(gizmo::hit_test(gizmo::Mode::Rotate, o, f, px(o, f.pivot + Vec3{0, -L, 0})) == gizmo::Handle::Z);
    gizmo::DrawList dl;
    gizmo::build(gizmo::Mode::Translate, o, f, gizmo::Handle::None, nullptr, dl);
    // X and Y arrows (+heads), one plane square and the centre square; no Z arrow.
    CHECK(dl.polygons.size() == 4);
    auto d = gizmo::Drag::begin(gizmo::Mode::Translate, gizmo::Handle::XY, o, f, px(o, f.pivot + Vec3{L * 0.34f, L * 0.34f, 0}));
    REQUIRE(d.has_value());
    check_vec(d->update(o, px(o, f.pivot + Vec3{L * 0.34f + 3, L * 0.34f - 2, 0}), nullptr).translation, {3, -2, 0});
    // Rotating in 2D turns counter-clockwise for a counter-clockwise drag (about +Z).
    auto r = gizmo::Drag::begin(gizmo::Mode::Rotate, gizmo::Handle::Z, o, f, px(o, f.pivot + Vec3{L, 0, 0}));
    REQUIRE(r.has_value());
    CHECK(r->update(o, px(o, f.pivot + Vec3{0, L, 0}), nullptr).angle_degrees == doctest::Approx(90).epsilon(0.01));
}

TEST_CASE("gizmo: draw lists highlight and isolate handles") {
    const gizmo::View v = perspective({6, 5, 8});
    const gizmo::Frame f{{0, 0, 0}, {}};
    gizmo::DrawList idle, hot;
    gizmo::build(gizmo::Mode::Translate, v, f, gizmo::Handle::None, nullptr, idle);
    gizmo::build(gizmo::Mode::Translate, v, f, gizmo::Handle::Y, nullptr, hot);
    CHECK(idle.lines.size() == hot.lines.size());
    CHECK(idle.lines.size() == 3 + 3 * 4 + 4);
    bool differs = false;
    for (std::size_t i = 0; i < idle.lines.size(); ++i) differs |= idle.lines[i].color != hot.lines[i].color;
    CHECK(differs);
    // While dragging only the active handle is drawn.
    const float L = handle_length(v, f.pivot);
    auto d = gizmo::Drag::begin(gizmo::Mode::Translate, gizmo::Handle::Y, v, f, px(v, {0, L * 0.7f, 0}));
    REQUIRE(d.has_value());
    gizmo::DrawList active;
    gizmo::build(gizmo::Mode::Translate, v, f, gizmo::Handle::Y, &*d, active);
    CHECK(active.lines.size() == 1);
    CHECK(active.polygons.size() == 1);
    // A rotate drag draws the swept angle.
    auto r = gizmo::Drag::begin(gizmo::Mode::Rotate, gizmo::Handle::Y, v, f, px(v, {L, 0, 0}));
    REQUIRE(r.has_value());
    r->update(v, px(v, {0, 0, -L}), nullptr);
    gizmo::DrawList rot;
    gizmo::build(gizmo::Mode::Rotate, v, f, gizmo::Handle::Y, &*r, rot);
    CHECK(rot.polygons.size() >= 8);
    CHECK(gizmo::snap_value(0.74f, 0.5f) == doctest::Approx(0.5f));
    CHECK(gizmo::snap_value(-0.76f, 0.5f) == doctest::Approx(-1.0f));
    CHECK(gizmo::snap_value(0.3f, 0.0f) == doctest::Approx(0.3f));
    CHECK(std::string(gizmo::handle_name(gizmo::Handle::ZX)) == "ZX");
}
