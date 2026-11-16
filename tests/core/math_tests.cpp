// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/core/math.hpp"

using namespace victus::math;

namespace {
bool near(Vec3 a, Vec3 b, float eps = 1e-4f) { return length(a - b) < eps; }
bool near(const Mat4& a, const Mat4& b, float eps = 1e-4f) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            if (std::fabs(a.c[i][j] - b.c[i][j]) > eps) return false;
    return true;
}
} // namespace

TEST_CASE("math: quaternion rotation matches matrix rotation") {
    const Quat q = Quat::axis_angle({0, 1, 0}, kPi / 2);
    CHECK(near(q.rotate({1, 0, 0}), {0, 0, -1}));
    CHECK(near(Mat4::rotation(q).transform_vector({1, 0, 0}), {0, 0, -1}));
    const Quat r = Quat::euler(0.3f, -1.1f, 0.7f);
    const Vec3 e = to_euler(r);
    CHECK(e.x == doctest::Approx(0.3f).epsilon(1e-4));
    CHECK(e.y == doctest::Approx(-1.1f).epsilon(1e-4));
    CHECK(e.z == doctest::Approx(0.7f).epsilon(1e-4));
}

TEST_CASE("math: inverse and decompose of TRS") {
    const Mat4 m = Mat4::trs({1, 2, 3}, Quat::euler(0.2f, 0.5f, -0.3f), {2, 3, 4});
    CHECK(near(m * inverse(m), Mat4::identity()));
    Vec3 t, s;
    Quat r;
    decompose(m, t, r, s);
    CHECK(near(t, {1, 2, 3}));
    CHECK(near(s, {2, 3, 4}));
    CHECK(near(Mat4::trs(t, r, s), m));
}

TEST_CASE("math: perspective maps near to 0 and far to 1") {
    const Mat4 p = Mat4::perspective(radians(60), 16.0f / 9.0f, 0.1f, 100.0f);
    const Vec4 n = p * Vec4{0, 0, -0.1f, 1};
    const Vec4 f = p * Vec4{0, 0, -100.0f, 1};
    CHECK(n.z / n.w == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(f.z / f.w == doctest::Approx(1.0f).epsilon(1e-5));
    const Mat4 o = Mat4::orthographic(-1, 1, -1, 1, 0, 10);
    CHECK((o * Vec4{0, 0, -10, 1}).z == doctest::Approx(1.0f));
}

TEST_CASE("math: look_at places target on -Z") {
    const Mat4 v = Mat4::look_at({0, 0, 5}, {0, 0, 0}, {0, 1, 0});
    CHECK(near(v.transform_point({0, 0, 0}), {0, 0, -5}));
}

TEST_CASE("math: frustum culling") {
    const Mat4 vp = Mat4::perspective(radians(90), 1, 0.1f, 50) * Mat4::look_at({0, 0, 0}, {0, 0, -1}, {0, 1, 0});
    const Frustum f = Frustum::from_matrix(vp);
    CHECK(f.intersects({{-1, -1, -11}, {1, 1, -9}}));
    CHECK_FALSE(f.intersects({{-1, -1, 9}, {1, 1, 11}}));    // behind
    CHECK_FALSE(f.intersects({{-1, -1, -70}, {1, 1, -60}})); // beyond far
    CHECK_FALSE(f.intersects({{30, -1, -11}, {32, 1, -9}})); // outside right
}

TEST_CASE("math: aabb transform and color packing") {
    Aabb box{{-1, -1, -1}, {1, 1, 1}};
    const Aabb moved = box.transformed(Mat4::translation({10, 0, 0}) * Mat4::scale({2, 1, 1}));
    CHECK(near(moved.min, {8, -1, -1}));
    CHECK(near(moved.max, {12, 1, 1}));
    const Color c = Color::hex(0xff8000);
    CHECK(Color::from_rgba8(c.rgba8()).g == doctest::Approx(128.0f / 255.0f));
    CHECK(linear_to_srgb(srgb_to_linear(0.5f)) == doctest::Approx(0.5f));
}
