// SPDX-License-Identifier: Apache-2.0
#include "victus/core/math.hpp"

namespace victus::math {

Quat Quat::euler(float pitch, float yaw, float roll) noexcept {
    return normalize(axis_angle({0, 1, 0}, yaw) * axis_angle({1, 0, 0}, pitch) * axis_angle({0, 0, 1}, roll));
}

Vec3 to_euler(Quat q) noexcept {
    // Inverse of Y*X*Z composition.
    const Mat4 m = Mat4::rotation(q);
    const float sp = -m.c[2].y; // -m[1][2] in row/col terms
    float pitch, yaw, roll;
    if (std::fabs(sp) < 0.9999f) {
        pitch = std::asin(clamp(sp, -1.0f, 1.0f));
        yaw = std::atan2(m.c[2].x, m.c[2].z);
        roll = std::atan2(m.c[0].y, m.c[1].y);
    } else {
        pitch = sp > 0 ? kPi * 0.5f : -kPi * 0.5f;
        yaw = std::atan2(-m.c[0].z, m.c[0].x);
        roll = 0.0f;
    }
    return {pitch, yaw, roll};
}

Quat slerp(Quat a, Quat b, float t) noexcept {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) {
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    if (d > 0.9995f) {
        return normalize(Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
    }
    const float theta = std::acos(d);
    const float s = std::sin(theta);
    const float wa = std::sin((1 - t) * theta) / s;
    const float wb = std::sin(t * theta) / s;
    return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}

Mat4 Mat4::rotation(Quat q) {
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    Mat4 m;
    m.c[0] = {1 - 2 * (yy + zz), 2 * (xy + wz), 2 * (xz - wy), 0};
    m.c[1] = {2 * (xy - wz), 1 - 2 * (xx + zz), 2 * (yz + wx), 0};
    m.c[2] = {2 * (xz + wy), 2 * (yz - wx), 1 - 2 * (xx + yy), 0};
    return m;
}

Mat4 Mat4::trs(Vec3 t, Quat r, Vec3 s) {
    Mat4 m = rotation(r);
    m.c[0] = m.c[0] * s.x;
    m.c[1] = m.c[1] * s.y;
    m.c[2] = m.c[2] * s.z;
    m.c[3] = {t, 1.0f};
    return m;
}

Mat4 Mat4::perspective(float fov_y, float aspect, float z_near, float z_far) {
    const float f = 1.0f / std::tan(fov_y * 0.5f);
    Mat4 m;
    m.c[0] = {f / aspect, 0, 0, 0};
    m.c[1] = {0, f, 0, 0};
    m.c[2] = {0, 0, z_far / (z_near - z_far), -1};
    m.c[3] = {0, 0, (z_near * z_far) / (z_near - z_far), 0};
    return m;
}

Mat4 Mat4::orthographic(float l, float r, float b, float t, float n, float f) {
    Mat4 m;
    m.c[0] = {2 / (r - l), 0, 0, 0};
    m.c[1] = {0, 2 / (t - b), 0, 0};
    m.c[2] = {0, 0, 1 / (n - f), 0};
    m.c[3] = {-(r + l) / (r - l), -(t + b) / (t - b), n / (n - f), 1};
    return m;
}

Mat4 Mat4::look_at(Vec3 eye, Vec3 target, Vec3 up) {
    const Vec3 f = normalize(target - eye);
    Vec3 s = cross(f, up);
    if (dot(s, s) < 1e-12f) s = cross(f, std::fabs(f.y) < 0.99f ? Vec3{0, 1, 0} : Vec3{1, 0, 0});
    s = normalize(s);
    const Vec3 u = cross(s, f);
    Mat4 m;
    m.c[0] = {s.x, u.x, -f.x, 0};
    m.c[1] = {s.y, u.y, -f.y, 0};
    m.c[2] = {s.z, u.z, -f.z, 0};
    m.c[3] = {-dot(s, eye), -dot(u, eye), dot(f, eye), 1};
    return m;
}

Mat4 transpose(const Mat4& m) noexcept {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r.c[i][j] = m.c[j][i];
    return r;
}

Mat4 inverse(const Mat4& mat) noexcept {
    const float* m = mat.data();
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    Mat4 r;
    if (std::fabs(det) < 1e-30f) return r;
    det = 1.0f / det;
    float* out = &r.c[0].x;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * det;
    return r;
}

void decompose(const Mat4& m, Vec3& t, Quat& r, Vec3& s) noexcept {
    t = m.c[3].xyz();
    s = {length(m.c[0].xyz()), length(m.c[1].xyz()), length(m.c[2].xyz())};
    if (dot(cross(m.c[0].xyz(), m.c[1].xyz()), m.c[2].xyz()) < 0) s.x = -s.x;
    const Vec3 c0 = s.x != 0 ? m.c[0].xyz() / s.x : Vec3{1, 0, 0};
    const Vec3 c1 = s.y != 0 ? m.c[1].xyz() / s.y : Vec3{0, 1, 0};
    const Vec3 c2 = s.z != 0 ? m.c[2].xyz() / s.z : Vec3{0, 0, 1};
    const float trace = c0.x + c1.y + c2.z;
    if (trace > 0) {
        const float k = std::sqrt(trace + 1.0f) * 2;
        r = {(c1.z - c2.y) / k, (c2.x - c0.z) / k, (c0.y - c1.x) / k, 0.25f * k};
    } else if (c0.x > c1.y && c0.x > c2.z) {
        const float k = std::sqrt(1.0f + c0.x - c1.y - c2.z) * 2;
        r = {0.25f * k, (c1.x + c0.y) / k, (c2.x + c0.z) / k, (c1.z - c2.y) / k};
    } else if (c1.y > c2.z) {
        const float k = std::sqrt(1.0f + c1.y - c0.x - c2.z) * 2;
        r = {(c1.x + c0.y) / k, 0.25f * k, (c2.y + c1.z) / k, (c2.x - c0.z) / k};
    } else {
        const float k = std::sqrt(1.0f + c2.z - c0.x - c1.y) * 2;
        r = {(c2.x + c0.z) / k, (c2.y + c1.z) / k, 0.25f * k, (c0.y - c1.x) / k};
    }
    r = normalize(r);
}

Aabb Aabb::transformed(const Mat4& m) const noexcept {
    if (!valid()) return *this;
    const Vec3 c = m.transform_point(center());
    const Vec3 e = extents();
    Vec3 ne;
    for (int i = 0; i < 3; ++i)
        ne[i] = std::fabs(m.c[0][i]) * e.x + std::fabs(m.c[1][i]) * e.y + std::fabs(m.c[2][i]) * e.z;
    return {c - ne, c + ne};
}

Frustum Frustum::from_matrix(const Mat4& vp) noexcept {
    const Mat4 t = transpose(vp); // rows of vp
    Frustum f;
    f.planes[0] = t.c[3] + t.c[0]; // left
    f.planes[1] = t.c[3] - t.c[0]; // right
    f.planes[2] = t.c[3] + t.c[1]; // bottom
    f.planes[3] = t.c[3] - t.c[1]; // top
    f.planes[4] = t.c[2];          // near (depth 0..1)
    f.planes[5] = t.c[3] - t.c[2]; // far
    for (auto& p : f.planes) {
        const float l = length(p.xyz());
        if (l > 0) p = p * (1.0f / l);
    }
    return f;
}

bool Frustum::intersects(const Aabb& box) const noexcept {
    const Vec3 c = box.center();
    const Vec3 e = box.extents();
    for (const auto& p : planes) {
        const float r = e.x * std::fabs(p.x) + e.y * std::fabs(p.y) + e.z * std::fabs(p.z);
        if (dot(p.xyz(), c) + p.w < -r) return false;
    }
    return true;
}

std::uint32_t Color::rgba8() const noexcept {
    auto q = [](float v) { return static_cast<std::uint32_t>(saturate(v) * 255.0f + 0.5f); };
    return q(r) | (q(g) << 8) | (q(b) << 16) | (q(a) << 24);
}

Color Color::from_rgba8(std::uint32_t p) noexcept {
    return {(p & 0xff) / 255.0f, ((p >> 8) & 0xff) / 255.0f, ((p >> 16) & 0xff) / 255.0f, ((p >> 24) & 0xff) / 255.0f};
}

float srgb_to_linear(float c) noexcept { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float linear_to_srgb(float c) noexcept { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }

} // namespace victus::math
