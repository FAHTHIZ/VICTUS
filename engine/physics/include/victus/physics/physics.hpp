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

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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

struct ColliderComponent {
    enum class Shape : std::uint8_t { Box, Sphere, Capsule, Cylinder };
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
    friend bool operator==(const ColliderComponent&, const ColliderComponent&) = default;
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

struct Collider2DComponent {
    enum class Shape : std::uint8_t { Box, Circle, Capsule };
    Shape shape{Shape::Box};
    math::Vec2 size{1, 1};       // box: full extents; capsule: width x total height
    float radius{0.5f};          // circle
    math::Vec2 offset;
    float rounding{0.0f};        // box corner radius
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

// Registers the reflected component schemas ("RigidBody", "Collider", "CharacterController",
// "Joint", "RigidBody2D", "Collider2D", "CharacterController2D", "Joint2D"). Idempotent.
void register_components();

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
    // Desired horizontal walking velocity (m/s, world space; Y is ignored). Persists until changed.
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

    void set_gravity(math::Vec3 g);
    void set_gravity_2d(math::Vec2 g);
    [[nodiscard]] math::Vec3 gravity() const;
    [[nodiscard]] math::Vec2 gravity_2d() const;

    [[nodiscard]] PhysicsStats stats() const;

    struct Impl;

private:
    explicit PhysicsWorld(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Installs "physics.step" in Phase::FixedUpdate. Install after systems whose fixed_update should
// run before the step (scripts). The world must outlive the simulation's use of it. The world is
// also published as an ECS resource (PhysicsWorld*) on the scene when the system first runs, so
// scripts and gameplay systems can find it with scene.world().resource<PhysicsWorld*>().
void add_physics_systems(gameplay::Simulation& sim, PhysicsWorld& world);

// Returns the physics world bound to a scene by add_physics_systems/bind, or null.
[[nodiscard]] PhysicsWorld* world_of(scene::Scene& scene);
void bind(scene::Scene& scene, PhysicsWorld* world);

// Collider wireframes (including character capsules and joint anchors/axes) computed from components only (no simulation needed): used by the editor
// to visualize colliders while authoring.
void collider_lines(scene::Scene& scene, std::vector<DebugLine>& out, Entity only = {});

} // namespace victus::physics
