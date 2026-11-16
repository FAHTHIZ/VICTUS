// SPDX-License-Identifier: Apache-2.0
// VICTUS math: right-handed, Y-up, column-major matrices, column vectors (M * v),
// clip-space depth in [0,1] (Vulkan/Metal/D3D convention).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace victus::math {

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kTau = 2.0f * kPi;
[[nodiscard]] constexpr float radians(float deg) noexcept { return deg * (kPi / 180.0f); }
[[nodiscard]] constexpr float degrees(float rad) noexcept { return rad * (180.0f / kPi); }
template <typename T>
[[nodiscard]] constexpr T lerp(T a, T b, float t) noexcept { return a + (b - a) * t; }
[[nodiscard]] constexpr float clamp(float v, float lo, float hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }
[[nodiscard]] constexpr float saturate(float v) noexcept { return clamp(v, 0.0f, 1.0f); }

struct Vec2 {
    float x{0}, y{0};
    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}
    constexpr explicit Vec2(float s) : x(s), y(s) {}
    constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator*(Vec2 o) const { return {x * o.x, y * o.y}; }
    constexpr Vec2 operator*(float s) const { return {x * s, y * s}; }
    constexpr Vec2 operator/(float s) const { return {x / s, y / s}; }
    constexpr Vec2 operator-() const { return {-x, -y}; }
    constexpr Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
    constexpr Vec2& operator-=(Vec2 o) { x -= o.x; y -= o.y; return *this; }
    constexpr Vec2& operator*=(float s) { x *= s; y *= s; return *this; }
    friend constexpr bool operator==(Vec2, Vec2) = default;
};
[[nodiscard]] constexpr float dot(Vec2 a, Vec2 b) noexcept { return a.x * b.x + a.y * b.y; }
[[nodiscard]] inline float length(Vec2 v) noexcept { return std::sqrt(dot(v, v)); }
[[nodiscard]] inline Vec2 normalize(Vec2 v) noexcept {
    const float l = length(v);
    return l > 0 ? v / l : Vec2{};
}

struct Vec3 {
    float x{0}, y{0}, z{0};
    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    constexpr explicit Vec3(float s) : x(s), y(s), z(s) {}
    constexpr Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(Vec3 o) const { return {x * o.x, y * o.y, z * o.z}; }
    constexpr Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    constexpr Vec3& operator+=(Vec3 o) { x += o.x; y += o.y; z += o.z; return *this; }
    constexpr Vec3& operator-=(Vec3 o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    constexpr Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    constexpr float& operator[](int i) { return (&x)[i]; }
    constexpr float operator[](int i) const { return (&x)[i]; }
    friend constexpr bool operator==(Vec3, Vec3) = default;
};
[[nodiscard]] constexpr Vec3 operator*(float s, Vec3 v) noexcept { return v * s; }
[[nodiscard]] constexpr float dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
[[nodiscard]] constexpr Vec3 cross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] inline float length(Vec3 v) noexcept { return std::sqrt(dot(v, v)); }
[[nodiscard]] inline Vec3 normalize(Vec3 v) noexcept {
    const float l = length(v);
    return l > 0 ? v / l : Vec3{};
}
[[nodiscard]] inline Vec3 min(Vec3 a, Vec3 b) noexcept { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
[[nodiscard]] inline Vec3 max(Vec3 a, Vec3 b) noexcept { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }

struct Vec4 {
    float x{0}, y{0}, z{0}, w{0};
    constexpr Vec4() = default;
    constexpr Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    constexpr Vec4(Vec3 v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
    constexpr Vec3 xyz() const { return {x, y, z}; }
    constexpr Vec4 operator+(Vec4 o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
    constexpr Vec4 operator-(Vec4 o) const { return {x - o.x, y - o.y, z - o.z, w - o.w}; }
    constexpr Vec4 operator*(float s) const { return {x * s, y * s, z * s, w * s}; }
    constexpr Vec4 operator*(Vec4 o) const { return {x * o.x, y * o.y, z * o.z, w * o.w}; }
    constexpr float& operator[](int i) { return (&x)[i]; }
    constexpr float operator[](int i) const { return (&x)[i]; }
    friend constexpr bool operator==(Vec4, Vec4) = default;
};
[[nodiscard]] constexpr float dot(Vec4 a, Vec4 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

struct Quat {
    float x{0}, y{0}, z{0}, w{1};
    constexpr Quat() = default;
    constexpr Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    static Quat axis_angle(Vec3 axis, float angle) noexcept {
        const Vec3 a = normalize(axis);
        const float s = std::sin(angle * 0.5f);
        return {a.x * s, a.y * s, a.z * s, std::cos(angle * 0.5f)};
    }
    // Intrinsic yaw (Y), pitch (X), roll (Z) in radians, applied Y * X * Z.
    static Quat euler(float pitch, float yaw, float roll) noexcept;
    constexpr Quat operator*(Quat q) const {
        return {w * q.x + x * q.w + y * q.z - z * q.y, w * q.y - x * q.z + y * q.w + z * q.x,
                w * q.z + x * q.y - y * q.x + z * q.w, w * q.w - x * q.x - y * q.y - z * q.z};
    }
    constexpr Vec3 rotate(Vec3 v) const {
        const Vec3 u{x, y, z};
        const Vec3 t = cross(u, v) * 2.0f;
        return v + t * w + cross(u, t);
    }
    constexpr Quat conjugate() const { return {-x, -y, -z, w}; }
    friend constexpr bool operator==(Quat, Quat) = default;
};
[[nodiscard]] inline Quat normalize(Quat q) noexcept {
    const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return l > 0 ? Quat{q.x / l, q.y / l, q.z / l, q.w / l} : Quat{};
}
[[nodiscard]] Quat slerp(Quat a, Quat b, float t) noexcept;
// Returns (pitch, yaw, roll) radians matching Quat::euler.
[[nodiscard]] Vec3 to_euler(Quat q) noexcept;

struct Mat4 {
    Vec4 c[4]{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}; // columns
    static constexpr Mat4 identity() { return {}; }
    constexpr Vec4& operator[](int i) { return c[i]; }
    constexpr const Vec4& operator[](int i) const { return c[i]; }
    constexpr Vec4 operator*(Vec4 v) const { return c[0] * v.x + c[1] * v.y + c[2] * v.z + c[3] * v.w; }
    constexpr Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (int i = 0; i < 4; ++i) r.c[i] = (*this) * o.c[i];
        return r;
    }
    constexpr Vec3 transform_point(Vec3 p) const { return ((*this) * Vec4(p, 1.0f)).xyz(); }
    constexpr Vec3 transform_vector(Vec3 v) const { return ((*this) * Vec4(v, 0.0f)).xyz(); }
    [[nodiscard]] const float* data() const { return &c[0].x; }
    static Mat4 translation(Vec3 t) {
        Mat4 m;
        m.c[3] = {t, 1.0f};
        return m;
    }
    static Mat4 scale(Vec3 s) {
        Mat4 m;
        m.c[0].x = s.x;
        m.c[1].y = s.y;
        m.c[2].z = s.z;
        return m;
    }
    static Mat4 rotation(Quat q);
    static Mat4 trs(Vec3 t, Quat r, Vec3 s);
    // Right-handed perspective, depth 0 (near) .. 1 (far).
    static Mat4 perspective(float fov_y, float aspect, float z_near, float z_far);
    // Right-handed orthographic, depth 0..1.
    static Mat4 orthographic(float left, float right, float bottom, float top, float z_near, float z_far);
    static Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up);
    friend constexpr bool operator==(const Mat4&, const Mat4&) = default;
};
[[nodiscard]] Mat4 transpose(const Mat4& m) noexcept;
[[nodiscard]] Mat4 inverse(const Mat4& m) noexcept;
// Decomposes an affine TRS matrix (no shear).
void decompose(const Mat4& m, Vec3& translation, Quat& rotation, Vec3& scale) noexcept;

struct Aabb {
    Vec3 min{std::numeric_limits<float>::max()};
    Vec3 max{-std::numeric_limits<float>::max()};
    [[nodiscard]] bool valid() const noexcept { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }
    void expand(Vec3 p) noexcept {
        min = math::min(min, p);
        max = math::max(max, p);
    }
    [[nodiscard]] Vec3 center() const noexcept { return (min + max) * 0.5f; }
    [[nodiscard]] Vec3 extents() const noexcept { return (max - min) * 0.5f; }
    [[nodiscard]] Aabb transformed(const Mat4& m) const noexcept;
};

// Frustum from a view-projection matrix; planes point inward (Gribb/Hartmann).
struct Frustum {
    Vec4 planes[6];
    static Frustum from_matrix(const Mat4& view_proj) noexcept;
    [[nodiscard]] bool intersects(const Aabb& box) const noexcept;
};

struct Color {
    float r{1}, g{1}, b{1}, a{1};
    constexpr Color() = default;
    constexpr Color(float r_, float g_, float b_, float a_ = 1.0f) : r(r_), g(g_), b(b_), a(a_) {}
    [[nodiscard]] constexpr Vec4 vec() const { return {r, g, b, a}; }
    [[nodiscard]] std::uint32_t rgba8() const noexcept;
    static Color from_rgba8(std::uint32_t packed) noexcept;
    static Color hex(std::uint32_t rgb, float alpha = 1.0f) noexcept {
        return {((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, alpha};
    }
    // sRGB-encoded 0xRRGGBB (as picked in art tools) decoded to linear light for 3D shading.
    static Color srgb(std::uint32_t rgb, float alpha = 1.0f) noexcept;
    friend constexpr bool operator==(Color, Color) = default;
};
[[nodiscard]] float srgb_to_linear(float c) noexcept;
[[nodiscard]] float linear_to_srgb(float c) noexcept;

} // namespace victus::math
