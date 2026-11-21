// SPDX-License-Identifier: Apache-2.0
// VICTUS physics: rigid bodies, colliders, queries and contact events for 3D (Jolt Physics) and
// 2D (Box2D) behind one VICTUS-owned API. Gameplay code, scripts and the editor only see the
// types in this header; the backends are private implementation details (ADR-0009).
//
// Authoring model (same for both dimensions):
// - A Collider (or Collider2D) gives an entity a collision shape. Without a RigidBody the
//   entity becomes static level geometry.
// - A RigidBody (RigidBody2D) makes the entity static, kinematic (moved by its Transform,
//   pushes dynamic bodies) or dynamic (simulated; the physics world owns its Transform).
// - Bodies live in world space. Children of moving parents are supported; dynamic bodies
//   write back a local transform relative to their parent.
// - Moving a dynamic body's Transform from gameplay code teleports it.
// - 2D bodies simulate in the XY plane; Transform.position.z and the Z rotation are preserved.
// - A CharacterController makes the entity a kinematic capsule that walks: it slides along
//   walls, climbs steps, snaps to the ground, respects a maximum slope and pushes dynamic bodies.
//   Its origin is at the feet. Gameplay drives it with move_character()/jump_character().
// - A Joint connects the entity's body to another entity's body (or to the world) with a fixed,
//   point (ball), hinge, slider or distance constraint, optionally limited, motorized and
//   breakable.
#pragma once
#include "victus/core/math.hpp"
#include "victus/core/result.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/scene/scene.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace victus::jobs {
class JobSystem;
}

namespace victus::physics {

using scene::Entity;

enum class BodyType : std::uint8_t { Static, Kinematic, Dynamic };

inline constexpr std::uint32_t kAllLayers = 0xFFFFu;
inline constexpr int kMaxLayers = 16;

// ---- 3D components --------------------------------------------------------------------------

struct RigidBodyComponent {
    BodyType type{BodyType::Dynamic};
    float mass{1.0f};            // kg (dynamic only)
    float linear_damping{0.05f};
    float angular_damping{0.05f};
    float gravity_scale{1.0f};
    bool continuous{false};      // swept collision for fast bodies
    bool lock_rotation{false};   // keeps the body upright (characters, pickups)
    bool start_awake{true};
    math::Vec3 linear_velocity;  // initial velocity when play starts
    math::Vec3 angular_velocity; // radians per second
    friend bool operator==(const RigidBodyComponent&, const RigidBodyComponent&) = default;
};

// Convex and mesh shapes are built from triangle geometry: the model mesh named by `mesh_asset` /
// `mesh_index`, or, when `mesh_asset` is nil, the entity's own ProceduralMesh, MeshRenderer model
// or MeshRenderer primitive (see CollisionMeshCache). `convex` wraps the points in a convex hull
// and works for every body type; `mesh` collides against the exact triangles and is meant for
// static and kinematic level geometry: a dynamic body with a `mesh` collider uses its convex hull
// instead (logged once). Both follow the entity's (non-uniform) scale.
struct ColliderComponent {
    enum class Shape : std::uint8_t { Box, Sphere, Capsule, Cylinder, Convex, Mesh };
    Shape shape{Shape::Box};
    math::Vec3 size{1, 1, 1};    // box: full extents
    float radius{0.5f};          // sphere, capsule, cylinder
    float height{1.0f};          // capsule/cylinder: total height along local Y
    math::Vec3 offset;           // local offset from the entity origin
    float friction{0.5f};
    float restitution{0.0f};
    bool is_trigger{false};      // reports overlaps, no collision response
    std::uint32_t layer{0};      // 0..15
    std::uint32_t mask{kAllLayers}; // layers this collider collides with
    Uuid mesh_asset;             // convex/mesh: model asset; nil = the entity's own mesh
    std::uint32_t mesh_index{0}; // convex/mesh: mesh within the model
    friend bool operator==(const ColliderComponent&, const ColliderComponent&) = default;
};

// Geometry of a convex or mesh collider in the mesh's local space (before the entity transform).
struct CollisionMesh {
    std::vector<math::Vec3> positions;
    std::vector<std::uint32_t> indices; // triangle list
    math::Aabb bounds;
};
// Builds a CollisionMesh, validating indices (out of range or not a multiple of three fail) and
// computing bounds. Degenerate triangles are kept; the backends drop them.
[[nodiscard]] Result<std::shared_ptr<const CollisionMesh>> make_collision_mesh(std::vector<math::Vec3> positions,
                                                                               std::vector<std::uint32_t> indices);

// Where convex/mesh collider geometry comes from: a model asset's mesh or a built-in primitive.
struct CollisionMeshSource {
    Uuid model;                  // nil = `primitive`
    std::uint32_t mesh_index{0};
    std::string primitive;       // cube|sphere|plane|cylinder (MeshRenderer primitives)
};
// Loads geometry for a source; returns null when it does not exist or cannot be loaded (log why).
// The asset layer provides one over the project's cooked models (assets::collision_mesh_provider).
using CollisionMeshProvider = std::function<std::shared_ptr<const CollisionMesh>(const CollisionMeshSource&)>;

// Resolves the geometry of convex/mesh colliders and caches it, so a model used by many colliders
// is loaded once. Shared by the physics world (bodies), navigation (navmesh input) and the editor
// collider overlay. Not thread-safe.
class CollisionMeshCache {
public:
    CollisionMeshCache() = default;
    explicit CollisionMeshCache(CollisionMeshProvider provider) : provider_(std::move(provider)) {}
    void set_provider(CollisionMeshProvider provider);
    [[nodiscard]] bool has_provider() const noexcept { return static_cast<bool>(provider_); }
    // Geometry for the entity's collider: `col.mesh_asset` when set, else the entity's
    // ProceduralMesh, else its MeshRenderer model mesh or primitive. Null when there is none or the
    // provider cannot load it (a failed source is not retried until invalidate()).
    [[nodiscard]] std::shared_ptr<const CollisionMesh> resolve(scene::Scene& scene, Entity e, const ColliderComponent& col);
    [[nodiscard]] std::shared_ptr<const CollisionMesh> load(const CollisionMeshSource& source);
    // Forgets cached geometry of one model (hot reload) or everything.
    void invalidate(const Uuid& model);
    void invalidate();
    [[nodiscard]] std::size_t size() const noexcept { return models_.size() + procedural_.size(); }

private:
    CollisionMeshProvider provider_;
    std::unordered_map<std::string, std::shared_ptr<const CollisionMesh>> models_;
    struct Procedural {
        std::weak_ptr<const scene::ProceduralMesh> source;
        std::shared_ptr<const CollisionMesh> mesh;
    };
    std::unordered_map<std::uint64_t, Procedural> procedural_;
};

// Kinematic walking capsule (Jolt CharacterVirtual). Entities with a CharacterController do not
// need a Collider; a Collider on the same entity is ignored.
struct CharacterControllerComponent {
    float radius{0.3f};          // capsule radius (m)
    float height{1.8f};          // total capsule height (m); the entity origin is at the feet
    float max_slope{50.0f};      // degrees; steeper ground is treated as a wall
    float step_height{0.35f};    // stairs climbed and ground snapping distance (m); 0 disables both
    float gravity_scale{1.0f};
    float mass{70.0f};           // kg, used when pushing dynamic bodies
    float push_force{200.0f};    // maximum force applied to dynamic bodies in the way (N)
    float air_control{0.3f};     // 0 = no steering in the air, 1 = full ground control
    std::uint32_t layer{0};
    std::uint32_t mask{kAllLayers};
    friend bool operator==(const CharacterControllerComponent&, const CharacterControllerComponent&) = default;
};

// A constraint between this entity's body and `connected` (or the world when null). Anchor and
// axis are in this entity's local space and are captured in world space when play starts.
struct JointComponent {
    enum class Type : std::uint8_t { Fixed, Point, Hinge, Slider, Distance };
    Type type{Type::Hinge};
    Uuid connected;              // entity with a body; nil = attached to the world
    math::Vec3 anchor;           // local attachment point
    math::Vec3 axis{0, 1, 0};    // local hinge/slider axis
    math::Vec3 connected_anchor; // distance joints: point local to `connected`, or a world point when nil
    bool limits{false};
    float min_limit{-45.0f};     // hinge: degrees; slider/distance: metres (distance without limits keeps
                                 // the authored distance)
    float max_limit{45.0f};
    bool motor{false};           // hinge/slider velocity motor
    float motor_speed{0.0f};     // hinge: degrees per second; slider: metres per second
    float motor_force{1000.0f};  // maximum motor torque (N*m) or force (N)
    float break_force{0.0f};     // N (N*m for rotation-only reaction); 0 = unbreakable
    bool collide_connected{false};
    friend bool operator==(const JointComponent&, const JointComponent&) = default;
};

// Wheeled vehicle (ADR-0166, GAP-24) on an entity with a dynamic RigidBody and a Collider (the
// chassis). Its wheels are direct children carrying VehicleWheel; each wheel's authored position
// is the wheel centre with the suspension fully extended, in the chassis' frame (+Z forward, +Y
// up, +X left). Physics writes the wheels' Transforms every step (suspension travel, steering and
// spin), so a wheel mesh whose axle is its local X axis turns correctly. Driven wheels on the same
// axle share a limited-slip differential and the engine torque is split evenly across axles; the
// gearbox shifts automatically. Drive it with PhysicsWorld::set_vehicle_input, or set
// `player_input` to read W/S/A/D, the arrow keys, Space (handbrake) and a gamepad.
struct VehicleComponent {
    float engine_torque{500.0f};      // N*m at the flywheel
    float min_rpm{1000.0f};
    float max_rpm{6000.0f};
    float shift_up_rpm{4000.0f};
    float shift_down_rpm{2000.0f};
    float differential_ratio{3.42f};
    float limited_slip_ratio{1.4f};   // max/min wheel speed on an axle before torque moves to the slow wheel
    float max_pitch_roll{60.0f};      // degrees the body may tilt from upright (180 = unlimited)
    float anti_roll{1000.0f};         // anti-roll bar stiffness per steered/driven axle (N/m); 0 = none
    bool player_input{false};         // drive from the keyboard and gamepad
    friend bool operator==(const VehicleComponent&, const VehicleComponent&) = default;
};

struct VehicleWheelComponent {
    float radius{0.35f};              // m
    float width{0.25f};               // m
    float suspension_length{0.4f};    // m from the attachment point to the wheel centre at full droop
    float suspension_travel{0.25f};   // m the wheel can rise from full droop
    float spring_frequency{1.5f};     // Hz
    float spring_damping{0.5f};       // 0..1
    float steer_degrees{0.0f};        // maximum steering angle; 0 = fixed
    bool driven{true};                // receives engine torque
    float brake_torque{1500.0f};      // N*m
    float handbrake_torque{0.0f};     // N*m (usually only the rear wheels)
    float grip{1.0f};                 // scales tyre friction
    friend bool operator==(const VehicleWheelComponent&, const VehicleWheelComponent&) = default;
};

// Articulated ragdoll (A.R.C. stage 1, ADR-0091) on the character root. The skeleton is the set of
// descendant entities carrying a RagdollBone; each becomes a Jolt body joined to its nearest bone
// ancestor by a swing-twist constraint whose pivot is the bone's origin. Bodies follow (Kinematic)
// or track (Powered: joint motors drive towards) the animated pose the bones had at the end of
// the previous frame, or simulate freely (Passive). In Powered and Passive the simulated pose is
// written back into the bones' local Transforms after animation (Phase::PostUpdate), mixed with the
// animated pose by `blend`. A CharacterController on the same entity ignores its own ragdoll.
struct RagdollComponent {
    enum class Mode : std::uint8_t { Off, Kinematic, Powered, Passive };
    Mode mode{Mode::Off};
    float total_mass{70.0f};      // kg shared by bones with mass 0, in proportion to their volume
    float motor_frequency{4.0f};  // Powered: joint response (Hz) for the whole limb a joint carries
    float motor_damping{1.0f};    // Powered: joint motor damping ratio
    float max_motor_torque{400.0f}; // Powered: N*m per joint, scaled by RagdollBone::strength
    float blend{1.0f};            // Powered/Passive: 0 = show the animation, 1 = show the simulation
    bool pin_root{false};         // Powered: the root bone follows the animation kinematically
    // Powered: fraction of each joint's gravity load cancelled by internal joint torques (equal and
    // opposite on parent and child, so the ragdoll as a whole still falls), letting soft motors
    // hold poses without sagging.
    float gravity_compensation{1.0f};
    float friction{0.6f};
    float linear_damping{0.05f};
    float angular_damping{0.05f};
    std::uint32_t layer{0};
    std::uint32_t mask{kAllLayers};
    // Local axis of the root the character faces (get-up heading, face-up/down test). -Z matches
    // the built-in movement behaviours; glTF/Mixamo rigs facing +Z under an unrotated root use +Z.
    math::Vec3 front{0, 0, -1};
    friend bool operator==(const RagdollComponent&, const RagdollComponent&) = default;
};

// One ragdoll body on a skeleton bone entity. The shape runs from the bone's origin towards its
// first child bone (or continues the parent direction on leaf bones).
struct RagdollBoneComponent {
    enum class Shape : std::uint8_t { Capsule, Box, Sphere };
    Shape shape{Shape::Capsule};
    float radius{0.08f};      // capsule/sphere radius, box half width (m)
    float length{0.0f};       // m; 0 = distance to the first child bone (2 * radius on leaves)
    float mass{0.0f};         // kg; 0 = share of RagdollComponent::total_mass
    float swing{45.0f};       // swing limit (degrees) about the bone's local Z axis
    float swing_plane{45.0f}; // swing limit (degrees) about the bone's local X axis
    float twist_min{-30.0f};  // twist limits around the bone direction (degrees)
    float twist_max{30.0f};
    float strength{1.0f};     // Powered: motor torque scale; 0 = limp joint
    friend bool operator==(const RagdollBoneComponent&, const RagdollBoneComponent&) = default;
};

// Adds RagdollBone components to the recognised bones of a humanoid skeleton under `root` (hips,
// spine, chest, head, upper/lower arms, hands, upper/lower legs and feet; names such as
// "mixamorig:LeftUpLeg", "thigh_l", "UpperArm.R" and the VICTUS starter characters' joints) with
// limits suited to each joint, and a RagdollComponent on `root` when it has none. Returns the
// number of bones configured (0 when no hips/pelvis bone was found).
std::size_t make_humanoid_ragdoll(scene::Scene& scene, Entity root);

// ---- 2D components --------------------------------------------------------------------------

struct RigidBody2DComponent {
    BodyType type{BodyType::Dynamic};
    float mass{1.0f};
    float linear_damping{0.0f};
    float angular_damping{0.05f};
    float gravity_scale{1.0f};
    bool fixed_rotation{false};
    bool bullet{false};          // continuous collision against other dynamic bodies
    bool start_awake{true};
    math::Vec2 linear_velocity;
    float angular_velocity{0};   // radians per second
    friend bool operator==(const RigidBody2DComponent&, const RigidBody2DComponent&) = default;
};

// polygon: any simple outline in `points` (either winding, concave allowed, at most
// kMaxPolygonPoints), split into convex pieces. chain: a line through `points` (`loop` closes it)
// that collides on one side only: an open chain on the left of its direction (ground drawn left to
// right is solid from above), a loop on its outside, or its inside when `inside` is set (cave and
// arena walls). Chains have no mass: dynamic bodies fill a loop as a polygon instead, and chains
// cannot be triggers. Points are in the entity's local space (scaled like the other shapes, plus
// `offset`).
struct Collider2DComponent {
    enum class Shape : std::uint8_t { Box, Circle, Capsule, Polygon, Chain };
    Shape shape{Shape::Box};
    math::Vec2 size{1, 1};       // box: full extents; capsule: width x total height
    float radius{0.5f};          // circle
    math::Vec2 offset;
    float rounding{0.0f};        // box corner radius
    std::vector<math::Vec2> points; // polygon / chain outline
    bool loop{true};             // chain: connect the last point to the first
    bool inside{false};          // chain loop: collide on the inside instead of the outside
    float friction{0.6f};
    float restitution{0.0f};
    bool is_trigger{false};
    // One-way platform: bodies and characters pass through from below and land on top. "Up" is the
    // entity's local +Y rotated into the world.
    bool one_way{false};
    std::uint32_t layer{0};
    std::uint32_t mask{kAllLayers};
    friend bool operator==(const Collider2DComponent&, const Collider2DComponent&) = default;
};

inline constexpr std::size_t kMaxPolygonPoints = 256;

// Splits a simple polygon (either winding, concave allowed, no self-intersections, at most
// kMaxPolygonPoints points) into convex counter-clockwise pieces of at most `max_vertices` points:
// ear clipping, then Hertel-Mehlhorn merging of triangles across diagonals. Duplicate and collinear
// points are dropped. Fails with a message on fewer than 3 distinct points, zero area,
// non-finite values or self-intersection.
[[nodiscard]] Result<std::vector<std::vector<math::Vec2>>> decompose_polygon(const std::vector<math::Vec2>& points,
                                                                            std::size_t max_vertices = 8);
// Twice the signed area (positive = counter-clockwise).
[[nodiscard]] float signed_area2(const std::vector<math::Vec2>& points) noexcept;
// True when two non-adjacent edges of the outline cross (closed: the last point joins the first).
[[nodiscard]] bool self_intersects(const std::vector<math::Vec2>& points, bool closed) noexcept;

// Kinematic 2D walker (a capsule centred on the entity origin, moved with Box2D's plane solver).
// It slides along walls, follows slopes up to max_slope, snaps down onto ground within
// snap_distance, climbs small ledges with its rounded bottom, rides moving platforms, lands on
// one-way platforms, pushes dynamic bodies and is seen by raycasts and triggers. A Collider2D or
// RigidBody2D on the same entity is ignored. Gameplay drives it with move_character()/jump_character().
struct CharacterController2DComponent {
    math::Vec2 size{0.8f, 1.0f};  // capsule width x total height (m)
    float max_slope{50.0f};       // degrees; steeper surfaces are walls
    float snap_distance{0.15f};   // keeps walking characters on downward slopes and steps (m)
    float gravity_scale{1.0f};
    float push_force{60.0f};      // maximum force on dynamic bodies in the way (N); 0 = no pushing
    float air_control{0.5f};      // 0 = no steering in the air, 1 = full ground control
    bool top_down{false};         // walk in the X/Y plane (move_character x and y), no gravity or ground
    std::uint32_t layer{0};
    std::uint32_t mask{kAllLayers};
    friend bool operator==(const CharacterController2DComponent&, const CharacterController2DComponent&) = default;
};

// A 2D constraint between this entity's body and `connected` (or the world when null). Anchors and
// the axis are in local space and captured in world space when the joint is created.
struct Joint2DComponent {
    enum class Type : std::uint8_t { Weld, Revolute, Prismatic, Distance, Wheel };
    Type type{Type::Revolute};
    Uuid connected;               // entity with a 2D body; nil = attached to the world
    math::Vec2 anchor;            // local attachment point on this entity
    math::Vec2 axis{1, 0};        // local slide axis (prismatic) or suspension axis (wheel)
    math::Vec2 connected_anchor;  // distance: point local to `connected`, or a world point when nil
    bool limits{false};
    float min_limit{-45.0f};      // revolute: degrees; prismatic/wheel/distance: metres
    float max_limit{45.0f};
    bool motor{false};
    float motor_speed{0.0f};      // revolute/wheel: degrees per second; prismatic/distance: m/s
    float motor_force{1000.0f};   // maximum motor torque (N*m) or force (N)
    bool spring{false};           // soft weld/revolute/prismatic/distance; the wheel always has one
    float spring_hertz{4.0f};
    float spring_damping{0.7f};
    float break_force{0.0f};      // N (or N*m for torque); 0 = unbreakable
    bool collide_connected{false};
    friend bool operator==(const Joint2DComponent&, const Joint2DComponent&) = default;
};

// Runtime-only static 2D geometry generated by other systems (tilemaps, procedural levels): boxes
// and closed edge loops in the entity's local XY space on one static body. Not saved or reflected;
// bump `revision` after changing them to rebuild the body. Raycasts and contacts report this
// entity.
//
// Each loop is a closed outline with the solid region on its left (counter-clockwise around
// solids, clockwise around holes). It becomes a one-sided Box2D chain loop that collides on the
// side away from the solid, so bodies slide across shared edges without the "ghost" bumps that
// separate boxes produce at their seams. Points closer than 7.5 mm (after scale) are merged and
// loops left with fewer than 3 points are skipped.
struct StaticShapes2DComponent {
    struct Box {
        math::Vec2 min, max;
        bool one_way{false}; // passable from below, like Collider2D::one_way
    };
    std::vector<Box> boxes;
    std::vector<std::vector<math::Vec2>> loops;
    float friction{0.6f};
    float restitution{0.0f};
    std::uint32_t layer{0};
    std::uint32_t mask{kAllLayers};
    std::uint64_t revision{0};
};

// Runtime-only static 3D height field generated by other systems (terrain): resolution^2 local
// heights, row-major with rows along +z; sample (x, z) sits at offset + (x * spacing.x,
// heights[z * resolution + x], z * spacing.y) in the entity's local space (translation and
// rotation of its world transform; scale ignored). Not saved or reflected; bump `revision`
// after changing it. Raycasts, contacts and characters report this entity.
//
// Incremental edits: after changing heights only inside a rectangle (same resolution, offset and
// spacing), include the rectangle in `dirty` and bump `height_revision` instead of `revision`.
// The physics world then rewrites just those samples of the existing shape (rounded out to
// Jolt's block size), wakes bodies above them and clears `dirty`. Heights outside the shape's
// encodable range fall back to a full rebuild, so producers that know their range (terrain:
// 0..size.y) set `height_range` to reserve it when the shape is first built.
struct HeightfieldRect {
    std::uint32_t x0{1}, z0{1}, x1{0}, z1{0}; // inclusive sample coordinates; empty when x1 < x0
    [[nodiscard]] bool empty() const noexcept { return x1 < x0 || z1 < z0; }
    void include(const HeightfieldRect& r) noexcept {
        if (r.empty()) return;
        if (empty()) {
            *this = r;
            return;
        }
        x0 = x0 < r.x0 ? x0 : r.x0;
        z0 = z0 < r.z0 ? z0 : r.z0;
        x1 = x1 > r.x1 ? x1 : r.x1;
        z1 = z1 > r.z1 ? z1 : r.z1;
    }
};

struct StaticHeightfieldComponent {
    std::uint32_t resolution{0};
    std::shared_ptr<const std::vector<float>> heights;
    math::Vec3 offset;
    math::Vec2 spacing{1, 1};
    float friction{0.8f};
    float restitution{0.0f};
    std::uint32_t layer{0};
    std::uint32_t mask{kAllLayers};
    std::uint64_t revision{0};
    math::Vec2 height_range{0, 0};   // min, max local height the shape must encode (ignored unless max > min)
    std::uint64_t height_revision{0};
    HeightfieldRect dirty;
};

// Registers the reflected component schemas ("RigidBody", "Collider", "CharacterController",
// "Joint", "Ragdoll", "RagdollBone", "RigidBody2D", "Collider2D", "CharacterController2D", "Joint2D"). Idempotent.
void register_components();


// ---- destruction ---------------------------------------------------------------------------------

// Pre-fractured destructible (Chaos/Blast-style Voronoi fracture). The convex hull of the entity's
// visible mesh (ProceduralMesh, MeshRenderer model or primitive; else its collider) is cut into
// `pieces` Voronoi cells seeded deterministically from `seed`, so the same settings always give
// the same fragments. A contact whose impulse estimate (ContactEvent::impulse) or a
// PhysicsWorld::damage call reaches `strength` breaks it: the entity is hidden, loses its body and
// gains one child entity per fragment (ProceduralMesh + copied MeshRenderer + Convex collider +
// RigidBody + DestructibleFragment). A dynamic original bursts into dynamic pieces that inherit
// its velocity. A static or kinematic original (walls, pillars) only frees the fragments within
// `damage_radius` of the impact; with `support`, fragments no longer connected through shared
// faces to a fragment resting on the original's base fall as well, and the rest stay static
// until hit again.
struct DestructibleComponent {
    std::uint32_t pieces{12};      // 2..256 Voronoi cells
    std::uint32_t seed{1};
    float strength{20.0f};         // N*s impulse that breaks it (or frees more pieces once broken)
    float damage_radius{0.6f};     // m: static originals free fragments this close to the impact
    float burst{1.5f};             // m/s outward speed given to freed fragments near the impact
    float density{600.0f};         // kg/m^3 for fragments of a static or kinematic original
    float debris_lifetime{0.0f};   // s before freed fragments are removed; 0 = keep
    std::uint32_t max_debris{64};  // freed fragments kept per destructible; the oldest go first
    bool support{true};            // static originals: unsupported fragments fall
    bool broken{false};            // saved: the fragments replace this entity
};
// One fragment of a broken destructible (a child entity of `owner`). The fragment geometry is not
// saved: it is regenerated deterministically from the owner's settings on load.
struct DestructibleFragmentComponent {
    Uuid owner;
    std::uint32_t piece{0};
    float age{0.0f}; // s since the fragment was freed
};

struct FractureSettings {
    std::uint32_t pieces{12};
    std::uint32_t seed{1};
};
// One Voronoi cell of a fractured convex volume, in the source's space. Faces are flat shaded
// (vertices are not shared between faces); `colors` mark interior (cut) faces darker.
struct FracturePiece {
    std::vector<math::Vec3> positions;
    std::vector<math::Vec3> normals;
    std::vector<math::Vec2> uvs;
    std::vector<std::uint32_t> colors;
    std::vector<std::uint32_t> indices;
    math::Vec3 centroid;
    float volume{0};
    math::Aabb bounds;
    std::vector<std::uint32_t> neighbours; // pieces sharing a face with this one (sorted)
};
struct Fracture {
    std::vector<FracturePiece> pieces;
    float volume{0}; // of the fractured hull (the pieces' volumes sum to it)
};
// Cuts the convex hull of `points` into Voronoi cells. When the points come from a convex triangle
// mesh (`indices`), its faces bound the hull exactly; otherwise (or when it is concave) the hull
// is approximated by a 90-direction discrete-orientation polytope. Fails on fewer than four
// non-coplanar points.
[[nodiscard]] Result<Fracture> fracture_convex(const std::vector<math::Vec3>& points,
                                               const std::vector<std::uint32_t>& indices,
                                               const FractureSettings& settings);

// ---- queries and events ----------------------------------------------------------------------

struct RaycastHit {
    Entity entity;
    math::Vec3 point;
    math::Vec3 normal;
    float distance{0};
};

struct ContactEvent {
    enum class Kind : std::uint8_t { Begin, End };
    Kind kind{Kind::Begin};
    Entity a, b;
    bool trigger{false};  // at least one side is a trigger
    bool is_2d{false};
    math::Vec3 point;     // world contact point (Begin only; zero for triggers)
    math::Vec3 normal;    // from a to b (Begin only)
    // Begin, 3D, non-trigger: closing speed along the normal when the bodies met (m/s) and the
    // impulse that stops it, speed times the pair's reduced mass (N*s; a static side counts as
    // infinitely heavy). An estimate taken before the solver runs; zero otherwise.
    float speed{0};
    float impulse{0};
};

struct CharacterState {
    bool valid{false};           // the entity has a simulated character
    bool grounded{false};        // standing on walkable ground
    bool on_steep_slope{false};  // touching ground steeper than max_slope
    math::Vec3 ground_normal;
    math::Vec3 velocity;         // m/s, after the last step
    Entity ground;               // entity stood on (null for world-less ground or in the air)
};

struct JointState {
    bool valid{false};           // the entity has a Joint that is (or was) simulated
    bool active{false};          // constraint is present and not broken
    bool broken{false};          // exceeded break_force; stays broken until the component changes
    float angle{0};              // hinge: degrees
    float position{0};           // slider: metres along the axis; distance: current distance
    float force{0};              // magnitude of the reaction force/torque in the last step
};

struct WaterContact {
    Entity water;          // null: not in water
    float submerged{0};    // fraction of the body's volume below the surface
    float surface{0};      // world height of the surface at the body
    float speed{0};        // body speed when sampled (m/s)
};

struct RagdollState {
    bool valid{false};             // the entity has a simulated ragdoll (mode != Off)
    RagdollComponent::Mode mode{RagdollComponent::Mode::Off};
    std::uint32_t bodies{0};
    bool awake{false};             // at least one body is active
    math::Vec3 center_of_mass;     // world space, after the last step
    math::Vec3 velocity;           // centre-of-mass velocity (m/s)
    float pose_error{0};           // mean distance (m) between bodies and their animated targets
    bool getting_up{false};        // a get-up blend (begin_ragdoll_get_up) is running
    float get_up_progress{0};      // 0..1 through that blend
};

// Makes `chassis` drivable: adds a box Collider when it has none, a dynamic 1,500 kg RigidBody
// (an existing one becomes dynamic) and a VehicleComponent. Children named like a wheel ("wheel",
// "tire", "tyre", case-insensitive) get VehicleWheel; when there are none, four wheel children are
// created at the corners of the box collider. Front wheels (local +Z) steer 30 degrees, rear
// wheels get the handbrake, all are driven. Returns the number of wheels (0 if `chassis` is dead).
std::size_t make_vehicle(scene::Scene& scene, Entity chassis);

// Driver controls for a vehicle; values are clamped.
struct VehicleInput {
    float forward{0};    // -1 (reverse) .. 1 (full throttle)
    float steer{0};      // -1 (left) .. 1 (right)
    float brake{0};      // 0 .. 1
    float handbrake{0};  // 0 .. 1
};

struct VehicleWheelState {
    Entity wheel;
    bool contact{false};
    float suspension{0};      // current suspension length (m)
    float steer_degrees{0};
    float rpm{0};             // wheel rotation speed
};

struct VehicleState {
    bool valid{false};        // the entity has a simulated vehicle
    float speed{0};           // m/s along the chassis' forward axis (negative when reversing)
    float engine_rpm{0};
    int gear{0};              // -1 reverse, 0 neutral, 1.. forward gears
    std::uint32_t wheels_in_contact{0};
    std::vector<VehicleWheelState> wheels;
    std::string error;        // why the vehicle is not simulated (no body, no wheels...)
};

// Keyboard/gamepad mapping used by VehicleComponent::player_input: W/Up throttle, S/Down brake then
// reverse, A/D or Left/Right and the left stick steer, Space or South handbrake, RT/LT throttle and
// brake. `speed` is the current forward speed (m/s).
[[nodiscard]] VehicleInput vehicle_input_from(const gameplay::InputSource* input, float speed);

// Result of PhysicsWorld::begin_ragdoll_get_up.
struct RagdollGetUp {
    bool started{false};
    bool face_up{false};      // lying on the back: play a get-up-from-back animation, else from front
    math::Vec3 position;      // world position the root was moved to (ground under the hips)
    math::Quat rotation;      // world rotation the root was given (yaw only)
    std::string reason;       // why it did not start
};

struct PhysicsSettings {
    math::Vec3 gravity{0, -9.81f, 0};
    math::Vec2 gravity_2d{0, -9.81f};
    std::uint32_t max_bodies{65536};
    std::uint32_t max_body_pairs{65536};
    std::uint32_t max_contact_constraints{16384};
    int collision_steps{1};      // 3D: collision steps per fixed step
    int substeps_2d{4};          // 2D: solver sub-steps per fixed step
    // Worker threads shared with the engine. Null runs the solvers on the calling thread.
    jobs::JobSystem* jobs{nullptr};
    // With `jobs`: convex and mesh colliders whose mesh has more triangles than this build their
    // Jolt shape in the background; the body is created on the first step after the build
    // finishes (`PhysicsStats::mesh_shapes_pending` counts builds in flight). 0 = always build
    // during the step that first sees the mesh.
    std::uint32_t async_mesh_triangles{4096};
    // Optional layer collision matrix: bit j of layer_matrix[i] lets layer i collide with j.
    // Applied in addition to per-collider masks. Default: all layers collide.
    std::uint32_t layer_matrix[kMaxLayers]{kAllLayers, kAllLayers, kAllLayers, kAllLayers, kAllLayers, kAllLayers,
                                           kAllLayers, kAllLayers, kAllLayers, kAllLayers, kAllLayers, kAllLayers,
                                           kAllLayers, kAllLayers, kAllLayers, kAllLayers};
};

struct PhysicsStats {
    std::uint32_t bodies_3d{0}, active_3d{0};
    std::uint32_t bodies_2d{0}, awake_2d{0};
    std::uint32_t characters_2d{0}, joints_2d{0};
    std::uint64_t steps{0};
    std::uint32_t contacts_last_step{0};
    double last_step_ms{0};
    std::uint64_t heightfield_builds{0};  // height-field shapes created (first sight or `revision` bumps)
    std::uint64_t heightfield_updates{0}; // in-place `dirty` rectangle updates
    std::uint64_t mesh_shapes_built{0};   // convex hulls and triangle meshes built (shared by bodies using one mesh)
    std::uint32_t mesh_shapes_pending{0}; // background shape builds in flight (bodies not created yet)
};

// Line segments for debug visualization (editor overlays, `victus run --physics-debug`).
struct DebugLine {
    math::Vec3 a, b;
    math::Color color;
};

class PhysicsWorld {
public:
    static Result<std::unique_ptr<PhysicsWorld>> create(const PhysicsSettings& settings = {});
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // Creates/updates/destroys backend bodies to match the scene's components and pushes
    // kinematic targets and teleports. Called by step(); call directly after loading to run
    // queries before the first step.
    void sync(scene::Scene& scene, float dt);
    // sync + one fixed step + write-back of dynamic bodies + contact events.
    void step(scene::Scene& scene, float dt);
    // Destroys every body (e.g. when a play session ends).
    void clear();

    // Contact/trigger events produced since the start of the current simulation tick (all fixed
    // steps of the tick when systems are installed; otherwise since the last begin_frame()),
    // sorted deterministically within each step.
    void begin_frame();
    [[nodiscard]] const std::vector<ContactEvent>& events() const noexcept;

    // ---- queries (3D) ----
    [[nodiscard]] std::optional<RaycastHit> raycast(math::Vec3 origin, math::Vec3 direction, float max_distance,
                                                    std::uint32_t mask = kAllLayers, Entity ignore = {}) const;
    // Every hit along the ray (not only the nearest), sorted by distance, at most `max_hits`.
    // Triggers are excluded as for raycast().
    [[nodiscard]] std::vector<RaycastHit> raycast_all(math::Vec3 origin, math::Vec3 direction, float max_distance,
                                                      std::uint32_t mask = kAllLayers, std::size_t max_hits = 16) const;
    [[nodiscard]] std::vector<Entity> overlap_sphere(math::Vec3 center, float radius, std::uint32_t mask = kAllLayers) const;
    [[nodiscard]] std::vector<Entity> overlap_box(math::Vec3 center, math::Vec3 half_extents, math::Quat rotation = {},
                                                  std::uint32_t mask = kAllLayers) const;
    // ---- queries (2D) ----
    [[nodiscard]] std::optional<RaycastHit> raycast_2d(math::Vec2 origin, math::Vec2 direction, float max_distance,
                                                       std::uint32_t mask = kAllLayers, Entity ignore = {}) const;
    [[nodiscard]] std::vector<Entity> overlap_circle_2d(math::Vec2 center, float radius, std::uint32_t mask = kAllLayers) const;
    [[nodiscard]] std::vector<Entity> overlap_box_2d(math::Vec2 center, math::Vec2 half_extents, float angle = 0,
                                                     std::uint32_t mask = kAllLayers) const;

    // ---- body control (no-ops for entities without a simulated body) ----
    [[nodiscard]] bool has_body(Entity e) const;
    [[nodiscard]] math::Vec3 linear_velocity(Entity e) const;
    void set_linear_velocity(Entity e, math::Vec3 v);
    [[nodiscard]] math::Vec3 angular_velocity(Entity e) const;
    void set_angular_velocity(Entity e, math::Vec3 v);
    void add_force(Entity e, math::Vec3 force);           // N, applied for the next step
    void add_impulse(Entity e, math::Vec3 impulse);       // N*s, immediate velocity change
    void add_torque(Entity e, math::Vec3 torque);
    void add_angular_impulse(Entity e, math::Vec3 impulse);

    // ---- characters (CharacterController in 3D, CharacterController2D in 2D) ----
    // Desired horizontal walking velocity (m/s, world space; Y is ignored, except by top-down
    // CharacterController2D characters, which walk along X and Y). Persists until changed.
    void move_character(Entity e, math::Vec3 velocity);
    // Jumps with the given upward speed on the next step. Returns false (and does nothing) when the
    // character is not grounded.
    bool jump_character(Entity e, float speed);
    [[nodiscard]] CharacterState character_state(Entity e) const;
    // 2D characters: ignore one-way platforms for `seconds` so the character drops through the one it
    // stands on. Returns false when the entity has no 2D character.
    bool drop_through(Entity e, float seconds = 0.25f);

    // ---- joints (Joint in 3D, Joint2D in 2D) ----
    [[nodiscard]] JointState joint_state(Entity e) const;
    // Writes the Joint/Joint2D motor fields and applies them to the live constraint without rebuilding it.
    void set_joint_motor(scene::Scene& scene, Entity e, bool enabled, float speed);

    // ---- vehicles (VehicleComponent, ADR-0166) ----
    // Persists until changed. Ignored for entities without a simulated vehicle.
    void set_vehicle_input(Entity e, const VehicleInput& input);
    [[nodiscard]] VehicleInput vehicle_input(Entity e) const;
    [[nodiscard]] VehicleState vehicle_state(Entity e) const;

    // ---- water (gameplay::WaterComponent volumes, ADR-0095) ----
    // Dynamic 3D bodies inside a water volume float by Jolt's submerged-volume buoyancy; this is
    // the state from the last step (water null when the body is not in water).
    [[nodiscard]] WaterContact water_contact(Entity e) const;

    // ---- ragdolls (Ragdoll + RagdollBone) ----
    // Bone entities with a simulated body also accept the body-control calls above (impulses on a
    // hit bone, velocities); raycasts and contacts report the bone entity.
    [[nodiscard]] RagdollState ragdoll_state(Entity root) const;
    // Writes RagdollComponent::mode; the change applies on the next step.
    void set_ragdoll_mode(scene::Scene& scene, Entity root, RagdollComponent::Mode mode);
    // The ragdoll root owning a bone entity's body, or null.
    [[nodiscard]] Entity ragdoll_of(Entity bone) const;
    // Recovery from a fallen (Passive or Powered) ragdoll (ADR-0158): moves the root to the ground
    // under the hips with the yaw a get-up animation starts from (face up: facing the feet; face
    // down: facing the head), switches the ragdoll to Kinematic and blends the bones from the
    // lying pose into whatever animation plays over `blend_seconds`. The caller starts the
    // matching get-up clip (RagdollGetUp::face_up) in the same frame. Applies immediately.
    RagdollGetUp begin_ragdoll_get_up(scene::Scene& scene, Entity root, float blend_seconds = 0.4f);
    // Writes simulated ragdoll poses into the bone Transforms (installed in Phase::PostUpdate by
    // add_physics_systems, after animation).
    void update_ragdoll_poses(scene::Scene& scene);

    // ---- destruction (Destructible + DestructibleFragment) ----
    // Damages a destructible (or the destructible owning a fragment) at a world point; applied in
    // the next step. Below the destructible's strength nothing happens.
    void damage(Entity e, math::Vec3 point, float impulse);
    // Regenerates fragment geometry of loaded broken destructibles (run by step; the editor can
    // call it to show them outside play). Returns the number of fragments rebuilt.
    std::size_t restore_fragments(scene::Scene& scene);

    void set_gravity(math::Vec3 g);
    void set_gravity_2d(math::Vec2 g);
    [[nodiscard]] math::Vec3 gravity() const;
    [[nodiscard]] math::Vec2 gravity_2d() const;

    [[nodiscard]] PhysicsStats stats() const;

    // Geometry for convex/mesh colliders. Without a provider those colliders only work with a
    // ProceduralMesh on the entity; others create no body (logged once per entity).
    void set_mesh_provider(CollisionMeshProvider provider);
    [[nodiscard]] CollisionMeshCache& mesh_cache() noexcept;
    // Drops cached geometry of a reimported model and rebuilds the bodies that use it.
    void invalidate_mesh(const Uuid& model);

    struct Impl;

private:
    explicit PhysicsWorld(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Installs "physics.step" in Phase::FixedUpdate and "physics.ragdoll_pose" in Phase::PostUpdate. Install after systems whose fixed_update should
// run before the step (scripts). The world must outlive the simulation's use of it. The world is
// also published as an ECS resource (PhysicsWorld*) on the scene when the system first runs, so
// scripts and gameplay systems can find it with scene.world().resource<PhysicsWorld*>().
void add_physics_systems(gameplay::Simulation& sim, PhysicsWorld& world);

// Returns the physics world bound to a scene by add_physics_systems/bind, or null.
[[nodiscard]] PhysicsWorld* world_of(scene::Scene& scene);
void bind(scene::Scene& scene, PhysicsWorld* world);

// Collider wireframes (including character capsules and joint anchors/axes) computed from components only (no simulation needed): used by the editor
// to visualize colliders while authoring.
// Convex and mesh colliders are drawn as their source triangles' edges (up to 4096 triangles per
// collider) when `meshes` can resolve them, else as nothing.
void collider_lines(scene::Scene& scene, std::vector<DebugLine>& out, Entity only = {}, CollisionMeshCache* meshes = nullptr);

} // namespace victus::physics
