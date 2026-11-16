// SPDX-License-Identifier: Apache-2.0
#include "victus/physics/physics.hpp"
#include "victus/scene/reflect.hpp"

#include <mutex>

namespace victus::physics {

void register_components() {
    static std::once_flag once;
    std::call_once(once, [] {
        using scene::FieldOptions;
        scene::Schema<RigidBodyComponent>("RigidBody", "Physics 3D")
            .enumeration(&RigidBodyComponent::type, "type", {"static", "kinematic", "dynamic"})
            .field(&RigidBodyComponent::mass, "mass", {.min = 0.001f, .max = 1e6f, .speed = 0.1f, .tooltip = "kg"})
            .field(&RigidBodyComponent::linear_damping, "linear_damping", {.min = 0, .max = 100, .speed = 0.01f})
            .field(&RigidBodyComponent::angular_damping, "angular_damping", {.min = 0, .max = 100, .speed = 0.01f})
            .field(&RigidBodyComponent::gravity_scale, "gravity_scale", {.min = -10, .max = 10, .speed = 0.05f})
            .field(&RigidBodyComponent::continuous, "continuous", {.tooltip = "Swept collision for fast bodies"})
            .field(&RigidBodyComponent::lock_rotation, "lock_rotation")
            .field(&RigidBodyComponent::start_awake, "start_awake")
            .field(&RigidBodyComponent::linear_velocity, "linear_velocity", {.speed = 0.05f})
            .field(&RigidBodyComponent::angular_velocity, "angular_velocity", {.speed = 0.05f})
            .commit();
        scene::Schema<ColliderComponent>("Collider", "Physics 3D")
            .enumeration(&ColliderComponent::shape, "shape", {"box", "sphere", "capsule", "cylinder"})
            .field(&ColliderComponent::size, "size", {.min = 0.001f, .max = 1e5f, .speed = 0.01f})
            .field(&ColliderComponent::radius, "radius", {.min = 0.001f, .max = 1e5f, .speed = 0.01f})
            .field(&ColliderComponent::height, "height", {.min = 0.001f, .max = 1e5f, .speed = 0.01f})
            .field(&ColliderComponent::offset, "offset", {.speed = 0.01f})
            .field(&ColliderComponent::friction, "friction", {.min = 0, .max = 10, .speed = 0.01f})
            .field(&ColliderComponent::restitution, "restitution", {.min = 0, .max = 1, .speed = 0.01f})
            .field(&ColliderComponent::is_trigger, "is_trigger")
            .field(&ColliderComponent::layer, "layer", {.min = 0, .max = 15})
            .field(&ColliderComponent::mask, "mask", {.hint = "bitmask16"})
            .commit();
        scene::Schema<CharacterControllerComponent>("CharacterController", "Physics 3D")
            .field(&CharacterControllerComponent::radius, "radius", {.min = 0.05f, .max = 100, .speed = 0.01f})
            .field(&CharacterControllerComponent::height, "height",
                   {.min = 0.1f, .max = 100, .speed = 0.01f, .tooltip = "Total height; the origin is at the feet"})
            .field(&CharacterControllerComponent::max_slope, "max_slope", {.min = 1, .max = 89, .speed = 0.5f, .tooltip = "degrees"})
            .field(&CharacterControllerComponent::step_height, "step_height", {.min = 0, .max = 10, .speed = 0.01f})
            .field(&CharacterControllerComponent::gravity_scale, "gravity_scale", {.min = -10, .max = 10, .speed = 0.05f})
            .field(&CharacterControllerComponent::mass, "mass", {.min = 0.1f, .max = 1e5f, .speed = 0.5f, .tooltip = "kg"})
            .field(&CharacterControllerComponent::push_force, "push_force", {.min = 0, .max = 1e6f, .speed = 1, .tooltip = "N"})
            .field(&CharacterControllerComponent::air_control, "air_control", {.min = 0, .max = 1, .speed = 0.01f})
            .field(&CharacterControllerComponent::layer, "layer", {.min = 0, .max = 15})
            .field(&CharacterControllerComponent::mask, "mask", {.hint = "bitmask16"})
            .commit();
        scene::Schema<JointComponent>("Joint", "Physics 3D")
            .enumeration(&JointComponent::type, "type", {"fixed", "point", "hinge", "slider", "distance"})
            .field(&JointComponent::connected, "connected", {.tooltip = "Other body; empty = world", .hint = "entity"})
            .field(&JointComponent::anchor, "anchor", {.speed = 0.01f})
            .field(&JointComponent::axis, "axis", {.speed = 0.01f})
            .field(&JointComponent::connected_anchor, "connected_anchor",
                   {.speed = 0.01f, .tooltip = "distance: point on the connected entity (local), or a world point"})
            .field(&JointComponent::limits, "limits")
            .field(&JointComponent::min_limit, "min_limit", {.speed = 0.1f, .tooltip = "hinge: degrees; slider/distance: m"})
            .field(&JointComponent::max_limit, "max_limit", {.speed = 0.1f, .tooltip = "hinge: degrees; slider/distance: m"})
            .field(&JointComponent::motor, "motor")
            .field(&JointComponent::motor_speed, "motor_speed", {.speed = 0.1f, .tooltip = "hinge: deg/s; slider: m/s"})
            .field(&JointComponent::motor_force, "motor_force", {.min = 0, .max = 1e9f, .speed = 1})
            .field(&JointComponent::break_force, "break_force", {.min = 0, .max = 1e9f, .speed = 1, .tooltip = "0 = unbreakable"})
            .field(&JointComponent::collide_connected, "collide_connected")
            .commit();
        scene::Schema<RigidBody2DComponent>("RigidBody2D", "Physics 2D")
            .enumeration(&RigidBody2DComponent::type, "type", {"static", "kinematic", "dynamic"})
            .field(&RigidBody2DComponent::mass, "mass", {.min = 0.001f, .max = 1e6f, .speed = 0.1f})
            .field(&RigidBody2DComponent::linear_damping, "linear_damping", {.min = 0, .max = 100, .speed = 0.01f})
            .field(&RigidBody2DComponent::angular_damping, "angular_damping", {.min = 0, .max = 100, .speed = 0.01f})
            .field(&RigidBody2DComponent::gravity_scale, "gravity_scale", {.min = -10, .max = 10, .speed = 0.05f})
            .field(&RigidBody2DComponent::fixed_rotation, "fixed_rotation")
            .field(&RigidBody2DComponent::bullet, "bullet")
            .field(&RigidBody2DComponent::start_awake, "start_awake")
            .field(&RigidBody2DComponent::linear_velocity, "linear_velocity", {.speed = 0.05f})
            .field(&RigidBody2DComponent::angular_velocity, "angular_velocity", {.speed = 0.05f})
            .commit();
        scene::Schema<Collider2DComponent>("Collider2D", "Physics 2D")
            .enumeration(&Collider2DComponent::shape, "shape", {"box", "circle", "capsule"})
            .field(&Collider2DComponent::size, "size", {.min = 0.001f, .max = 1e5f, .speed = 0.01f})
            .field(&Collider2DComponent::radius, "radius", {.min = 0.001f, .max = 1e5f, .speed = 0.01f})
            .field(&Collider2DComponent::offset, "offset", {.speed = 0.01f})
            .field(&Collider2DComponent::rounding, "rounding", {.min = 0, .max = 1e4f, .speed = 0.01f})
            .field(&Collider2DComponent::friction, "friction", {.min = 0, .max = 10, .speed = 0.01f})
            .field(&Collider2DComponent::restitution, "restitution", {.min = 0, .max = 1, .speed = 0.01f})
            .field(&Collider2DComponent::is_trigger, "is_trigger")
            .field(&Collider2DComponent::layer, "layer", {.min = 0, .max = 15})
            .field(&Collider2DComponent::mask, "mask", {.hint = "bitmask16"})
            .commit();
    });
}

} // namespace victus::physics
