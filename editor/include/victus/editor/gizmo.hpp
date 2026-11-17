// SPDX-License-Identifier: Apache-2.0
// Viewport transform gizmos: move, rotate and scale handles with hit testing, drag solving,
// snapping and screen-space draw geometry. The module only depends on VICTUS math, so the
// editor draws its shapes with the UI toolkit and tests drive it with plain pixel positions.
//
// Conventions: pixels have their origin at the top-left of the viewport; the view-projection
// matrix maps world space to clip space with depth 0 (near) .. 1 (far), as everywhere in VICTUS.
// Handles keep a constant on-screen size (Options::size_pixels) at any distance or zoom.
#pragma once
#include "victus/core/math.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace victus::editor::gizmo {

enum class Mode : std::uint8_t { Translate, Rotate, Scale };
// World: handles follow the world axes. Local: they follow the frame's orientation (the active
// entity's world rotation). Scale always works in Local.
enum class Space : std::uint8_t { World, Local };
// X/Y/Z: an axis (or the ring around it). XY/YZ/ZX: a plane handle (translate). View: the
// centre square (move in the view plane, uniform scale) or the outer ring (rotate about the
// view direction).
enum class Handle : std::uint8_t { None, X, Y, Z, XY, YZ, ZX, View };
[[nodiscard]] const char* mode_name(Mode m) noexcept;
[[nodiscard]] const char* handle_name(Handle h) noexcept;

struct Snap {
    float translate{0.5f};       // world units per step
    float rotate_degrees{15.0f}; // degrees per step
    float scale{0.1f};           // scale factor step
};

struct Options {
    float size_pixels{96.0f};   // axis length on screen
    float pick_pixels{8.0f};    // hit distance around lines
    float line_thickness{2.5f};
};

// A camera as seen by the gizmo: one view-projection matrix and the viewport size in pixels.
// Works for perspective and orthographic (2D) cameras alike.
class View {
public:
    View() = default;
    View(const math::Mat4& view_projection, math::Vec2 viewport_size);
    [[nodiscard]] math::Vec2 size() const noexcept { return size_; }
    [[nodiscard]] bool orthographic() const noexcept { return ortho_; }
    // Viewport pixel of a world point; nullopt when the point is behind the camera.
    [[nodiscard]] std::optional<math::Vec2> project(math::Vec3 world) const noexcept;
    // World-space ray through a pixel (origin on the near plane, unit direction).
    struct Ray {
        math::Vec3 origin, direction;
    };
    [[nodiscard]] Ray ray(math::Vec2 pixel) const noexcept;
    // World length of one pixel at the depth of `p` (constant for orthographic views).
    [[nodiscard]] float world_per_pixel(math::Vec3 p) const noexcept;
    // Unit direction from the camera through `p`.
    [[nodiscard]] math::Vec3 view_direction(math::Vec3 p) const noexcept;

private:
    math::Mat4 vp_, inv_vp_;
    math::Vec2 size_{1, 1};
    bool ortho_{false};
};

// Where the gizmo sits: the pivot (the active entity's world position) and the orientation of
// its handles (identity for World space).
struct Frame {
    math::Vec3 pivot;
    math::Quat orientation;
    [[nodiscard]] math::Vec3 axis(int i) const noexcept;
};

// The handle under a pixel, or Handle::None.
[[nodiscard]] Handle hit_test(Mode mode, const View& view, const Frame& frame, math::Vec2 pixel, const Options& opt = {});

// Result of a drag, relative to the state when the drag began.
struct Delta {
    math::Vec3 translation;     // world units (Translate)
    math::Quat rotation;        // world-space rotation about the pivot (Rotate)
    float angle_degrees{0.0f};  // signed angle of `rotation` about its handle axis
    math::Vec3 scale{1, 1, 1};  // factors along the frame axes (Scale)
};

// An active drag. begin() returns nullopt when `handle` cannot be dragged from this view
// (e.g. an axis pointing straight at the camera).
class Drag {
public:
    [[nodiscard]] static std::optional<Drag> begin(Mode mode, Handle handle, const View& view, const Frame& frame,
                                                   math::Vec2 pixel, const Options& opt = {});
    // Updates the drag for the mouse at `pixel`; `snap` rounds the result to the snap steps.
    Delta update(const View& view, math::Vec2 pixel, const Snap* snap);
    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    [[nodiscard]] Handle handle() const noexcept { return handle_; }
    [[nodiscard]] const Frame& frame() const noexcept { return frame_; }
    [[nodiscard]] const Delta& delta() const noexcept { return delta_; }
    // The rotation axis of a Rotate drag (world space).
    [[nodiscard]] math::Vec3 rotation_axis() const noexcept { return axis_; }
    // The unit direction from the pivot to the grab point of a Rotate drag (world space).
    [[nodiscard]] math::Vec3 rotation_start() const noexcept { return rotate_ref_; }

private:
    Mode mode_{Mode::Translate};
    Handle handle_{Handle::None};
    Frame frame_;
    math::Vec3 axis_;           // drag axis or rotation axis / plane normal
    math::Vec3 plane_u_, plane_v_; // plane handles: the two in-plane frame axes
    math::Vec3 start_point_;    // grab point on the axis line / plane
    math::Vec2 start_pixel_;
    float handle_length_{1.0f}; // world length of the handle at drag start
    // Rotate: plane intersection (false: screen tangent fallback for edge-on rings).
    bool rotate_plane_{true};
    math::Vec3 rotate_ref_;     // grab direction in the ring plane
    math::Vec2 tangent_px_;     // screen tangent of the ring at the grab point
    float radius_px_{1.0f};
    float accumulated_{0.0f};   // radians, unwrapped
    float last_angle_{0.0f};
    Delta delta_;
};

// Screen-space geometry for drawing a gizmo (colours are 0xAABBGGRR, as ImGui expects).
struct DrawList {
    struct Line {
        math::Vec2 a, b;
        std::uint32_t color;
        float thickness;
    };
    struct Polygon { // convex, filled
        std::vector<math::Vec2> points;
        std::uint32_t color;
    };
    std::vector<Line> lines;
    std::vector<Polygon> polygons;
    void clear() {
        lines.clear();
        polygons.clear();
    }
};
// `hot` is highlighted (hover), `active` while dragging (other handles are dimmed). For an
// active Rotate drag, the swept angle is drawn as a pie slice.
void build(Mode mode, const View& view, const Frame& frame, Handle hot, const Drag* active, DrawList& out,
           const Options& opt = {});

// Short human-readable summary of a drag, e.g. "Move 1.50, 0.00, 0.00" or "Rotate 45.0 deg".
[[nodiscard]] std::string describe(Mode mode, const Delta& d);

// Rounds `v` to the nearest multiple of `step` (step <= 0 leaves v unchanged).
[[nodiscard]] float snap_value(float v, float step) noexcept;

} // namespace victus::editor::gizmo
