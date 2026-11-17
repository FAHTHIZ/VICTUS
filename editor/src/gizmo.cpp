// SPDX-License-Identifier: Apache-2.0
#include "victus/editor/gizmo.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

namespace victus::editor::gizmo {

using math::Vec2;
using math::Vec3;
using math::Vec4;

namespace {

constexpr float kAxisStart = 0.18f;    // handle lines start this far from the pivot (fraction of L)
constexpr float kPlaneLo = 0.22f, kPlaneHi = 0.46f;
constexpr float kHideAxisDot = 0.985f; // axis nearly along the view direction: hidden
constexpr float kHidePlaneDot = 0.2f;  // plane nearly edge-on: hidden
constexpr float kHideRingDot = 0.05f;  // ring nearly edge-on: hidden
constexpr float kRingPlaneDot = 0.25f; // below this a ring drag uses the screen tangent
constexpr float kViewRingScale = 1.18f;
constexpr float kCenterHalfPx = 7.0f;
constexpr int kRingSegments = 64;

constexpr std::uint32_t rgba(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a = 255) {
    return (a << 24) | (b << 16) | (g << 8) | r;
}
constexpr std::uint32_t kAxisColor[3] = {rgba(232, 72, 72), rgba(112, 200, 72), rgba(72, 132, 238)};
constexpr std::uint32_t kViewColor = rgba(225, 225, 225);
constexpr std::uint32_t kHotColor = rgba(255, 212, 64);

std::uint32_t with_alpha(std::uint32_t c, std::uint32_t a) { return (c & 0x00ffffffu) | (a << 24); }

int axis_index(Handle h) {
    switch (h) {
    case Handle::X: return 0;
    case Handle::Y: return 1;
    case Handle::Z: return 2;
    default: return -1;
    }
}
Handle axis_handle(int i) { return i == 0 ? Handle::X : i == 1 ? Handle::Y : Handle::Z; }
// Plane handles: the two in-plane axes and the normal axis.
struct PlaneAxes {
    Handle handle;
    int u, v, n;
};
constexpr PlaneAxes kPlanes[3] = {{Handle::XY, 0, 1, 2}, {Handle::YZ, 1, 2, 0}, {Handle::ZX, 2, 0, 1}};
const PlaneAxes* plane_of(Handle h) {
    for (const auto& p : kPlanes)
        if (p.handle == h) return &p;
    return nullptr;
}

float dist_to_segment(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab = b - a;
    const float len2 = math::dot(ab, ab);
    const float t = len2 > 1e-12f ? math::clamp(math::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return math::length(p - (a + ab * t));
}

bool inside_convex(Vec2 p, const Vec2* pts, int n) {
    float sign = 0.0f;
    for (int i = 0; i < n; ++i) {
        const Vec2 a = pts[i], b = pts[(i + 1) % n];
        const float c = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        if (std::fabs(c) < 1e-9f) continue;
        if (sign == 0.0f) sign = c;
        else if ((c > 0) != (sign > 0)) return false;
    }
    return true;
}

// Closest point parameter along the line p0 + s*u (u unit) to the ray.
std::optional<float> closest_on_line(Vec3 p0, Vec3 u, const View::Ray& ray) {
    const Vec3 w0 = p0 - ray.origin;
    const float b = math::dot(u, ray.direction);
    const float d = math::dot(u, w0), e = math::dot(ray.direction, w0);
    const float denom = 1.0f - b * b;
    if (denom < 1e-6f) return std::nullopt;
    return (b * e - d) / denom;
}

std::optional<Vec3> intersect_plane(const View::Ray& ray, Vec3 point, Vec3 normal) {
    const float denom = math::dot(ray.direction, normal);
    if (std::fabs(denom) < 1e-6f) return std::nullopt;
    const float t = math::dot(point - ray.origin, normal) / denom;
    return ray.origin + ray.direction * t;
}

void ring_basis(Vec3 n, Vec3& u, Vec3& v) {
    u = math::normalize(math::cross(n, std::fabs(n.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}));
    v = math::cross(n, u);
}

struct Context {
    const View& view;
    const Frame& frame;
    const Options& opt;
    Vec2 center;
    float L;
    Vec3 vd;
};

std::optional<Context> make_context(const View& view, const Frame& frame, const Options& opt) {
    const auto c = view.project(frame.pivot);
    if (!c) return std::nullopt;
    return Context{view, frame, opt, *c, view.world_per_pixel(frame.pivot) * opt.size_pixels, view.view_direction(frame.pivot)};
}

bool axis_visible(const Context& c, int i) { return std::fabs(math::dot(c.frame.axis(i), c.vd)) < kHideAxisDot; }
bool plane_visible(const Context& c, const PlaneAxes& p) {
    return std::fabs(math::dot(c.frame.axis(p.n), c.vd)) > kHidePlaneDot;
}
bool ring_visible(const Context& c, int i) { return std::fabs(math::dot(c.frame.axis(i), c.vd)) > kHideRingDot; }

std::array<Vec3, 4> plane_quad(const Context& c, const PlaneAxes& p) {
    const Vec3 u = c.frame.axis(p.u) * c.L, v = c.frame.axis(p.v) * c.L, o = c.frame.pivot;
    return {o + u * kPlaneLo + v * kPlaneLo, o + u * kPlaneHi + v * kPlaneLo, o + u * kPlaneHi + v * kPlaneHi,
            o + u * kPlaneLo + v * kPlaneHi};
}

bool project_all(const View& view, const Vec3* pts, int n, Vec2* out) {
    for (int i = 0; i < n; ++i) {
        const auto p = view.project(pts[i]);
        if (!p) return false;
        out[i] = *p;
    }
    return true;
}

// Ring sample k of axis `n`; `front` is false for samples on the far half (hidden behind the pivot).
Vec3 ring_point(const Context& c, Vec3 u, Vec3 v, int k, bool* front) {
    const float a = math::kTau * static_cast<float>(k) / kRingSegments;
    const Vec3 d = u * std::cos(a) + v * std::sin(a);
    if (front) *front = c.view.orthographic() || math::dot(d, c.vd) <= 0.1f;
    return c.frame.pivot + d * c.L;
}

} // namespace

const char* mode_name(Mode m) noexcept {
    switch (m) {
    case Mode::Translate: return "Move";
    case Mode::Rotate: return "Rotate";
    case Mode::Scale: return "Scale";
    }
    return "?";
}

const char* handle_name(Handle h) noexcept {
    switch (h) {
    case Handle::None: return "None";
    case Handle::X: return "X";
    case Handle::Y: return "Y";
    case Handle::Z: return "Z";
    case Handle::XY: return "XY";
    case Handle::YZ: return "YZ";
    case Handle::ZX: return "ZX";
    case Handle::View: return "View";
    }
    return "?";
}

float snap_value(float v, float step) noexcept { return step > 0.0f ? std::round(v / step) * step : v; }

// ---- View ---------------------------------------------------------------------------------------

View::View(const math::Mat4& vp, Vec2 size) : vp_(vp), inv_vp_(math::inverse(vp)), size_(size) {
    ortho_ = std::fabs(vp.c[0].w) < 1e-7f && std::fabs(vp.c[1].w) < 1e-7f && std::fabs(vp.c[2].w) < 1e-7f &&
             std::fabs(vp.c[3].w - 1.0f) < 1e-6f;
}

std::optional<Vec2> View::project(Vec3 p) const noexcept {
    const Vec4 clip = vp_ * Vec4(p, 1.0f);
    if (clip.w <= 1e-6f) return std::nullopt;
    const float nx = clip.x / clip.w, ny = clip.y / clip.w;
    return Vec2{(nx * 0.5f + 0.5f) * size_.x, (0.5f - ny * 0.5f) * size_.y};
}

namespace {
Vec3 unproject(const math::Mat4& inv, float nx, float ny, float nz) {
    const Vec4 v = inv * Vec4{nx, ny, nz, 1.0f};
    return v.xyz() / v.w;
}
} // namespace

View::Ray View::ray(Vec2 pixel) const noexcept {
    const float nx = pixel.x / size_.x * 2.0f - 1.0f, ny = 1.0f - pixel.y / size_.y * 2.0f;
    const Vec3 a = unproject(inv_vp_, nx, ny, 0.0f), b = unproject(inv_vp_, nx, ny, 1.0f);
    return {a, math::normalize(b - a)};
}

float View::world_per_pixel(Vec3 p) const noexcept {
    const Vec4 clip = vp_ * Vec4(p, 1.0f);
    const float w = std::fabs(clip.w) > 1e-6f ? std::fabs(clip.w) : 1.0f;
    const float nx = clip.x / w, ny = clip.y / w, nz = clip.z / w;
    const Vec3 a = unproject(inv_vp_, nx, ny, nz), b = unproject(inv_vp_, nx + 2.0f / size_.x, ny, nz);
    return math::length(b - a);
}

Vec3 View::view_direction(Vec3 p) const noexcept {
    if (const auto px = project(p); px && !ortho_) return ray(*px).direction;
    return ray(size_ * 0.5f).direction;
}

Vec3 Frame::axis(int i) const noexcept {
    return orientation.rotate(i == 0 ? Vec3{1, 0, 0} : i == 1 ? Vec3{0, 1, 0} : Vec3{0, 0, 1});
}

// ---- hit testing --------------------------------------------------------------------------------

Handle hit_test(Mode mode, const View& view, const Frame& frame, Vec2 pixel, const Options& opt) {
    const auto ctx = make_context(view, frame, opt);
    if (!ctx) return Handle::None;
    const Context& c = *ctx;
    const Vec2 dc = pixel - c.center;
    if (mode != Mode::Rotate && std::fabs(dc.x) <= kCenterHalfPx + 1 && std::fabs(dc.y) <= kCenterHalfPx + 1)
        return Handle::View;
    if (mode == Mode::Translate) {
        for (const auto& p : kPlanes) {
            if (!plane_visible(c, p)) continue;
            const auto q = plane_quad(c, p);
            Vec2 s[4];
            if (project_all(view, q.data(), 4, s) && inside_convex(pixel, s, 4)) return p.handle;
        }
    }
    Handle best = Handle::None;
    float best_d = opt.pick_pixels;
    if (mode == Mode::Rotate) {
        for (int i = 0; i < 3; ++i) {
            if (!ring_visible(c, i)) continue;
            Vec3 u, v;
            ring_basis(frame.axis(i), u, v);
            bool f0 = false, f1 = false;
            Vec3 p0 = ring_point(c, u, v, 0, &f0);
            for (int k = 1; k <= kRingSegments; ++k) {
                const Vec3 p1 = ring_point(c, u, v, k, &f1);
                if (f0 && f1) {
                    const auto a = view.project(p0), b = view.project(p1);
                    if (a && b) {
                        if (const float d = dist_to_segment(pixel, *a, *b); d < best_d) {
                            best_d = d;
                            best = axis_handle(i);
                        }
                    }
                }
                p0 = p1;
                f0 = f1;
            }
        }
        if (best != Handle::None) return best;
        const float r = opt.size_pixels * kViewRingScale;
        if (std::fabs(math::length(dc) - r) < opt.pick_pixels) return Handle::View;
        return Handle::None;
    }
    for (int i = 0; i < 3; ++i) {
        if (!axis_visible(c, i)) continue;
        const Vec3 a3 = frame.pivot + frame.axis(i) * (c.L * kAxisStart), b3 = frame.pivot + frame.axis(i) * c.L;
        const auto a = view.project(a3), b = view.project(b3);
        if (!a || !b) continue;
        if (const float d = dist_to_segment(pixel, *a, *b); d < best_d) {
            best_d = d;
            best = axis_handle(i);
        }
    }
    return best;
}

// ---- dragging -----------------------------------------------------------------------------------

std::optional<Drag> Drag::begin(Mode mode, Handle handle, const View& view, const Frame& frame, Vec2 pixel,
                                const Options& opt) {
    if (handle == Handle::None) return std::nullopt;
    const auto ctx = make_context(view, frame, opt);
    if (!ctx) return std::nullopt;
    const Context& c = *ctx;
    Drag d;
    d.mode_ = mode;
    d.handle_ = handle;
    d.frame_ = frame;
    d.start_pixel_ = pixel;
    d.handle_length_ = c.L;
    d.radius_px_ = opt.size_pixels;
    const View::Ray ray = view.ray(pixel);
    const int ai = axis_index(handle);

    if (mode == Mode::Rotate) {
        d.axis_ = handle == Handle::View ? -c.vd : frame.axis(ai);
        if (handle != Handle::View && ai < 0) return std::nullopt;
        Vec3 u, v;
        ring_basis(d.axis_, u, v);
        d.rotate_plane_ = std::fabs(math::dot(d.axis_, c.vd)) > kRingPlaneDot;
        if (d.rotate_plane_) {
            const auto hit = intersect_plane(ray, frame.pivot, d.axis_);
            if (!hit || math::length(*hit - frame.pivot) < c.L * 1e-3f) return std::nullopt;
            d.rotate_ref_ = math::normalize(*hit - frame.pivot);
        } else {
            // Edge-on ring: grab the ring point nearest the mouse on screen and drag along its
            // screen-space tangent.
            float best = std::numeric_limits<float>::max();
            for (int k = 0; k < kRingSegments; ++k) {
                const Vec3 p = ring_point(c, u, v, k, nullptr);
                if (const auto s = view.project(p)) {
                    if (const float dist = math::length(*s - pixel); dist < best) {
                        best = dist;
                        d.rotate_ref_ = math::normalize(p - frame.pivot);
                    }
                }
            }
            const Vec3 g = frame.pivot + d.rotate_ref_ * c.L;
            const Vec3 t = math::cross(d.axis_, d.rotate_ref_) * (c.L * 0.05f);
            const auto s0 = view.project(g), s1 = view.project(g + t);
            if (!s0 || !s1 || math::length(*s1 - *s0) < 1e-4f) return std::nullopt;
            d.tangent_px_ = math::normalize(*s1 - *s0);
        }
        return d;
    }
    if (handle == Handle::View) {
        d.axis_ = c.vd;
        if (mode == Mode::Translate) {
            const auto hit = intersect_plane(ray, frame.pivot, c.vd);
            if (!hit) return std::nullopt;
            d.start_point_ = *hit;
        }
        return d;
    }
    if (const PlaneAxes* p = plane_of(handle)) {
        if (mode != Mode::Translate) return std::nullopt;
        d.axis_ = frame.axis(p->n);
        d.plane_u_ = frame.axis(p->u);
        d.plane_v_ = frame.axis(p->v);
        const auto hit = intersect_plane(ray, frame.pivot, d.axis_);
        if (!hit || std::fabs(math::dot(ray.direction, d.axis_)) < 0.05f) return std::nullopt;
        d.start_point_ = *hit;
        return d;
    }
    d.axis_ = frame.axis(ai);
    if (std::fabs(math::dot(d.axis_, c.vd)) > 0.995f) return std::nullopt;
    const auto s = closest_on_line(frame.pivot, d.axis_, ray);
    if (!s) return std::nullopt;
    d.start_point_ = frame.pivot + d.axis_ * *s;
    return d;
}

Delta Drag::update(const View& view, Vec2 pixel, const Snap* snap) {
    const View::Ray ray = view.ray(pixel);
    Delta out;
    switch (mode_) {
    case Mode::Translate:
        if (handle_ == Handle::View) {
            const auto hit = intersect_plane(ray, start_point_, axis_);
            if (!hit) return delta_;
            Vec3 t = *hit - start_point_;
            if (snap)
                for (int i = 0; i < 3; ++i) t[i] = snap_value(t[i], snap->translate);
            out.translation = t;
        } else if (plane_of(handle_)) {
            const auto hit = intersect_plane(ray, start_point_, axis_);
            if (!hit) return delta_;
            const Vec3 diff = *hit - start_point_;
            float du = math::dot(diff, plane_u_), dv = math::dot(diff, plane_v_);
            if (snap) {
                du = snap_value(du, snap->translate);
                dv = snap_value(dv, snap->translate);
            }
            out.translation = plane_u_ * du + plane_v_ * dv;
        } else {
            const auto s = closest_on_line(start_point_, axis_, ray);
            if (!s) return delta_;
            out.translation = axis_ * (snap ? snap_value(*s, snap->translate) : *s);
        }
        break;
    case Mode::Scale: {
        if (handle_ == Handle::View) {
            const float px = (pixel.x - start_pixel_.x) - (pixel.y - start_pixel_.y);
            float f = 1.0f + px / std::max(radius_px_, 1.0f);
            if (snap) f = 1.0f + snap_value(f - 1.0f, snap->scale);
            f = std::max(f, 0.001f);
            out.scale = {f, f, f};
            break;
        }
        const auto s = closest_on_line(start_point_, axis_, ray);
        if (!s) return delta_;
        float g = math::dot(start_point_ - frame_.pivot, axis_);
        if (std::fabs(g) < handle_length_ * 0.05f) g = handle_length_;
        float f = (g + *s) / g;
        if (snap) f = 1.0f + snap_value(f - 1.0f, snap->scale);
        f = std::max(f, 0.001f);
        out.scale[axis_index(handle_)] = f;
        break;
    }
    case Mode::Rotate: {
        if (rotate_plane_) {
            const auto hit = intersect_plane(ray, frame_.pivot, axis_);
            if (!hit) return delta_;
            Vec3 r = *hit - frame_.pivot;
            r = r - axis_ * math::dot(r, axis_);
            if (math::length(r) < handle_length_ * 1e-3f) return delta_;
            const float angle = std::atan2(math::dot(math::cross(rotate_ref_, r), axis_), math::dot(rotate_ref_, r));
            float diff = angle - last_angle_;
            while (diff > math::kPi) diff -= math::kTau;
            while (diff < -math::kPi) diff += math::kTau;
            accumulated_ += diff;
            last_angle_ = angle;
        } else {
            accumulated_ = math::dot(pixel - start_pixel_, tangent_px_) / std::max(radius_px_, 1.0f);
        }
        float deg = math::degrees(accumulated_);
        if (snap) deg = snap_value(deg, snap->rotate_degrees);
        out.angle_degrees = deg;
        out.rotation = math::Quat::axis_angle(axis_, math::radians(deg));
        break;
    }
    }
    delta_ = out;
    return out;
}

// ---- drawing ------------------------------------------------------------------------------------

void build(Mode mode, const View& view, const Frame& frame, Handle hot, const Drag* active, DrawList& out,
           const Options& opt) {
    const auto ctx = make_context(view, frame, opt);
    if (!ctx) return;
    const Context& c = *ctx;
    const Handle shown = active ? active->handle() : Handle::None;
    const auto visible = [&](Handle h) { return !active || shown == h; };
    const auto color_of = [&](Handle h, std::uint32_t base) { return (h == hot || h == shown) ? kHotColor : base; };
    const float th = opt.line_thickness;

    if (mode == Mode::Rotate) {
        for (int i = 0; i < 3; ++i) {
            const Handle h = axis_handle(i);
            if (!visible(h) || !ring_visible(c, i)) continue;
            Vec3 u, v;
            ring_basis(frame.axis(i), u, v);
            const std::uint32_t col = color_of(h, kAxisColor[i]);
            bool f0 = false, f1 = false;
            Vec3 p0 = ring_point(c, u, v, 0, &f0);
            for (int k = 1; k <= kRingSegments; ++k) {
                const Vec3 p1 = ring_point(c, u, v, k, &f1);
                if (f0 && f1) {
                    const auto a = view.project(p0), b = view.project(p1);
                    if (a && b) out.lines.push_back({*a, *b, col, th});
                }
                p0 = p1;
                f0 = f1;
            }
        }
        if (visible(Handle::View)) {
            const float r = opt.size_pixels * kViewRingScale;
            const std::uint32_t col = color_of(Handle::View, kViewColor);
            for (int k = 0; k < kRingSegments; ++k) {
                const float a0 = math::kTau * k / kRingSegments, a1 = math::kTau * (k + 1) / kRingSegments;
                out.lines.push_back({c.center + Vec2{std::cos(a0), std::sin(a0)} * r,
                                     c.center + Vec2{std::cos(a1), std::sin(a1)} * r, col, th * 0.8f});
            }
        }
        if (active) {
            // Swept angle as a translucent pie (triangle fan, each slice convex).
            const Vec3 axis = active->rotation_axis();
            const float total = math::radians(active->delta().angle_degrees);
            const int steps = std::max(1, static_cast<int>(std::fabs(total) / (math::kTau / kRingSegments)));
            const float r = active->handle() == Handle::View ? 0.95f : 1.0f;
            const std::uint32_t fill = with_alpha(kHotColor, 70);
            const Vec3 start_dir = active->rotation_start();
            for (int k = 0; k < steps; ++k) {
                const float a0 = total * k / steps, a1 = total * (k + 1) / steps;
                const Vec3 d0 = math::Quat::axis_angle(axis, a0).rotate(start_dir) * (c.L * r);
                const Vec3 d1 = math::Quat::axis_angle(axis, a1).rotate(start_dir) * (c.L * r);
                const auto p0 = view.project(frame.pivot + d0), p1 = view.project(frame.pivot + d1);
                if (p0 && p1) out.polygons.push_back({{c.center, *p0, *p1}, fill});
            }
        }
        return;
    }

    // Translate / Scale: axis lines with arrow heads (translate) or boxes (scale).
    for (int i = 0; i < 3; ++i) {
        const Handle h = axis_handle(i);
        if (!visible(h) || !axis_visible(c, i)) continue;
        const std::uint32_t col = color_of(h, kAxisColor[i]);
        const Vec3 ax = frame.axis(i);
        float tip_scale = 1.0f;
        if (active && mode == Mode::Scale) tip_scale = active->delta().scale[i];
        const auto a = view.project(frame.pivot + ax * (c.L * kAxisStart));
        const auto tip = view.project(frame.pivot + ax * (c.L * tip_scale));
        if (!a || !tip) continue;
        const Vec2 dir = math::normalize(*tip - *a);
        const Vec2 perp{-dir.y, dir.x};
        if (mode == Mode::Translate) {
            const Vec2 base = *tip - dir * 16.0f;
            out.lines.push_back({*a, base, col, th});
            out.polygons.push_back({{*tip, base + perp * 6.0f, base - perp * 6.0f}, col});
        } else {
            out.lines.push_back({*a, *tip, col, th});
            const float s = 5.5f;
            out.polygons.push_back({{*tip + Vec2{-s, -s}, *tip + Vec2{s, -s}, *tip + Vec2{s, s}, *tip + Vec2{-s, s}}, col});
        }
    }
    if (mode == Mode::Translate) {
        for (const auto& p : kPlanes) {
            if (!visible(p.handle) || !plane_visible(c, p)) continue;
            const auto q = plane_quad(c, p);
            Vec2 s[4];
            if (!project_all(view, q.data(), 4, s)) continue;
            const bool lit = p.handle == hot || p.handle == shown;
            const std::uint32_t col = lit ? with_alpha(kHotColor, 150) : with_alpha(kAxisColor[p.n], 90);
            out.polygons.push_back({{s[0], s[1], s[2], s[3]}, col});
            for (int k = 0; k < 4; ++k) out.lines.push_back({s[k], s[(k + 1) % 4], with_alpha(col, 220), 1.0f});
        }
    }
    if (visible(Handle::View)) {
        const float s = kCenterHalfPx;
        const std::uint32_t col = color_of(Handle::View, kViewColor);
        const Vec2 q[4] = {c.center + Vec2{-s, -s}, c.center + Vec2{s, -s}, c.center + Vec2{s, s}, c.center + Vec2{-s, s}};
        out.polygons.push_back({{q[0], q[1], q[2], q[3]}, with_alpha(col, Handle::View == hot ? 200 : 90)});
        for (int k = 0; k < 4; ++k) out.lines.push_back({q[k], q[(k + 1) % 4], col, 1.5f});
    }
}

std::string describe(Mode mode, const Delta& d) {
    char buf[128];
    switch (mode) {
    case Mode::Translate:
        std::snprintf(buf, sizeof buf, "Move %.3f, %.3f, %.3f", d.translation.x, d.translation.y, d.translation.z);
        break;
    case Mode::Rotate: std::snprintf(buf, sizeof buf, "Rotate %.1f deg", d.angle_degrees); break;
    case Mode::Scale: std::snprintf(buf, sizeof buf, "Scale %.3f, %.3f, %.3f", d.scale.x, d.scale.y, d.scale.z); break;
    }
    return buf;
}

} // namespace victus::editor::gizmo
