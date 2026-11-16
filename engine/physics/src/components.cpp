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
