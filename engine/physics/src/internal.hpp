// SPDX-License-Identifier: Apache-2.0
// Private interfaces shared by the physics backends.
#pragma once
#include "victus/physics/physics.hpp"

#include <cmath>
#include <memory>
#include <optional>
#include <vector>

namespace victus::physics::detail {

struct Pose {
    math::Vec3 position;
    math::Quat rotation;
    math::Vec3 scale{1, 1, 1};
};

// World-space pose of an entity computed from its Transform chain (not WorldTransform, which
// may be a frame old while gameplay code is running).
Pose world_pose(const scene::Scene& scene, Entity e);
// Writes a world-space position/rotation back into the entity's local Transform.
void write_world_pose(scene::Scene& scene, Entity e, math::Vec3 position, math::Quat rotation);

[[nodiscard]] inline bool nearly_equal(math::Vec3 a, math::Vec3 b, float eps = 1e-5f) {
    return std::abs(a.x - b.x) <= eps && std::abs(a.y - b.y) <= eps && std::abs(a.z - b.z) <= eps;
}
[[nodiscard]] inline bool nearly_equal(math::Quat a, math::Quat b, float eps = 1e-5f) {
    const float d = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return d >= 1.0f - eps;
}
[[nodiscard]] inline math::Vec3 abs(math::Vec3 v) { return {std::abs(v.x), std::abs(v.y), std::abs(v.z)}; }
// Angle around +Z of a rotation (exact for pure Z rotations; projects others onto Z).
[[nodiscard]] inline float z_angle(math::Quat q) { return 2.0f * std::atan2(q.z, q.w); }
[[nodiscard]] inline math::Quat with_z_angle(math::Quat original, float angle) {
    // Keep any X/Y tilt authored on 2D entities (rare) by replacing only the twist about Z.
    const float current = z_angle(original);
    return math::normalize(math::Quat::axis_angle({0, 0, 1}, angle - current) * original);
}

[[nodiscard]] inline std::uint64_t encode(Entity e) noexcept {
    return (static_cast<std::uint64_t>(e.generation) << 32) | (static_cast<std::uint64_t>(e.index) + 1u);
}
[[nodiscard]] inline Entity decode(std::uint64_t v) noexcept {
    if (v == 0) return {};
    return Entity{static_cast<std::uint32_t>((v & 0xFFFFFFFFu) - 1u), static_cast<std::uint32_t>(v >> 32)};
}

void sort_events(std::vector<ContactEvent>& events);

// ---- 3D backend (Jolt) ----
class World3D {
public:
    static std::unique_ptr<World3D> create(const PhysicsSettings& settings);
    virtual ~World3D() = default;
    virtual void sync(scene::Scene& scene, float dt) = 0;
    virtual void step(scene::Scene& scene, float dt, std::vector<ContactEvent>& events) = 0;
    virtual void clear() = 0;
    [[nodiscard]] virtual bool has_body(Entity e) const = 0;
    [[nodiscard]] virtual std::optional<RaycastHit> raycast(math::Vec3 o, math::Vec3 d, float max, std::uint32_t mask,
                                                            Entity ignore) const = 0;
    [[nodiscard]] virtual std::vector<Entity> overlap_sphere(math::Vec3 c, float r, std::uint32_t mask) const = 0;
    [[nodiscard]] virtual std::vector<Entity> overlap_box(math::Vec3 c, math::Vec3 h, math::Quat q,
                                                          std::uint32_t mask) const = 0;
    [[nodiscard]] virtual math::Vec3 linear_velocity(Entity e) const = 0;
    virtual void set_linear_velocity(Entity e, math::Vec3 v) = 0;
    [[nodiscard]] virtual math::Vec3 angular_velocity(Entity e) const = 0;
    virtual void set_angular_velocity(Entity e, math::Vec3 v) = 0;
    virtual void add_force(Entity e, math::Vec3 f) = 0;
    virtual void add_impulse(Entity e, math::Vec3 i) = 0;
    virtual void add_torque(Entity e, math::Vec3 t) = 0;
    virtual void add_angular_impulse(Entity e, math::Vec3 i) = 0;
    virtual void move_character(Entity e, math::Vec3 v) = 0;
    virtual bool jump_character(Entity e, float speed) = 0;
    [[nodiscard]] virtual CharacterState character_state(Entity e) const = 0;
    [[nodiscard]] virtual JointState joint_state(Entity e) const = 0;
    virtual void set_joint_motor(Entity e, bool enabled, float speed) = 0;
    virtual void set_gravity(math::Vec3 g) = 0;
    [[nodiscard]] virtual math::Vec3 gravity() const = 0;
    virtual void stats(PhysicsStats& out) const = 0;
};

// ---- 2D backend (Box2D) ----
class World2D {
public:
    static std::unique_ptr<World2D> create(const PhysicsSettings& settings);
    virtual ~World2D() = default;
    virtual void sync(scene::Scene& scene, float dt) = 0;
    virtual void step(scene::Scene& scene, float dt, std::vector<ContactEvent>& events) = 0;
    virtual void clear() = 0;
    [[nodiscard]] virtual bool has_body(Entity e) const = 0;
    [[nodiscard]] virtual std::optional<RaycastHit> raycast(math::Vec2 o, math::Vec2 d, float max, std::uint32_t mask,
                                                            Entity ignore) const = 0;
    [[nodiscard]] virtual std::vector<Entity> overlap_circle(math::Vec2 c, float r, std::uint32_t mask) const = 0;
    [[nodiscard]] virtual std::vector<Entity> overlap_box(math::Vec2 c, math::Vec2 h, float angle,
                                                          std::uint32_t mask) const = 0;
    [[nodiscard]] virtual math::Vec3 linear_velocity(Entity e) const = 0;
    virtual void set_linear_velocity(Entity e, math::Vec3 v) = 0;
    [[nodiscard]] virtual math::Vec3 angular_velocity(Entity e) const = 0;
    virtual void set_angular_velocity(Entity e, math::Vec3 v) = 0;
    virtual void add_force(Entity e, math::Vec3 f) = 0;
    virtual void add_impulse(Entity e, math::Vec3 i) = 0;
    virtual void add_torque(Entity e, math::Vec3 t) = 0;
    virtual void add_angular_impulse(Entity e, math::Vec3 i) = 0;
    virtual void set_gravity(math::Vec2 g) = 0;
    [[nodiscard]] virtual math::Vec2 gravity() const = 0;
    virtual void stats(PhysicsStats& out) const = 0;
};

} // namespace victus::physics::detail
