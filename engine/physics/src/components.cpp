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
            .enumeration(&ColliderComponent::shape, "shape", {"box", "sphere", "capsule", "cylinder", "convex", "mesh"})
            .field(&ColliderComponent::size, "size", {.min = 0.001f, .max = 1e5f, .speed = 0.01f})
            .field(&ColliderComponent::radius, "radius", {.min = 0.001f, .max = 1e5f, .speed = 0.01f})
            .field(&ColliderComponent::height, "height", {.min = 0.001f, .max = 1e5f, .speed = 0.01f})
            .field(&ColliderComponent::offset, "offset", {.speed = 0.01f})
            .field(&ColliderComponent::friction, "friction", {.min = 0, .max = 10, .speed = 0.01f})
            .field(&ColliderComponent::restitution, "restitution", {.min = 0, .max = 1, .speed = 0.01f})
            .field(&ColliderComponent::is_trigger, "is_trigger")
            .field(&ColliderComponent::layer, "layer", {.min = 0, .max = 15})
            .field(&ColliderComponent::mask, "mask", {.hint = "bitmask16"})
            .field(&ColliderComponent::mesh_asset, "mesh_asset",
                   {.tooltip = "convex/mesh: model to collide with; empty = this entity's own mesh", .hint = "model"})
            .field(&ColliderComponent::mesh_index, "mesh_index", {.min = 0, .max = 4096, .tooltip = "convex/mesh: mesh within the model"})
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
        scene::Schema<VehicleComponent>("Vehicle", "Physics 3D")
            .field(&VehicleComponent::engine_torque, "engine_torque", {.min = 0, .max = 1e5f, .speed = 1, .tooltip = "N*m"})
            .field(&VehicleComponent::min_rpm, "min_rpm", {.min = 1, .max = 30000, .speed = 10})
            .field(&VehicleComponent::max_rpm, "max_rpm", {.min = 1, .max = 30000, .speed = 10})
            .field(&VehicleComponent::shift_up_rpm, "shift_up_rpm", {.min = 1, .max = 30000, .speed = 10})
            .field(&VehicleComponent::shift_down_rpm, "shift_down_rpm", {.min = 1, .max = 30000, .speed = 10})
            .field(&VehicleComponent::differential_ratio, "differential_ratio", {.min = 0.01f, .max = 20, .speed = 0.01f})
            .field(&VehicleComponent::limited_slip_ratio, "limited_slip_ratio",
                   {.min = 1, .max = 1e6f, .speed = 0.01f, .tooltip = "Max/min wheel speed on an axle; large = open differential"})
            .field(&VehicleComponent::max_pitch_roll, "max_pitch_roll", {.min = 0, .max = 180, .speed = 1, .tooltip = "degrees; 180 = unlimited"})
            .field(&VehicleComponent::anti_roll, "anti_roll", {.min = 0, .max = 1e6f, .speed = 10, .tooltip = "N/m per axle; 0 = none"})
            .field(&VehicleComponent::player_input, "player_input", {.tooltip = "W/S/A/D, arrows, Space handbrake, gamepad"})
            .commit();
        scene::Schema<VehicleWheelComponent>("VehicleWheel", "Physics 3D")
            .field(&VehicleWheelComponent::radius, "radius", {.min = 0.01f, .max = 10, .speed = 0.01f})
            .field(&VehicleWheelComponent::width, "width", {.min = 0.01f, .max = 10, .speed = 0.01f})
            .field(&VehicleWheelComponent::suspension_length, "suspension_length",
                   {.min = 0, .max = 10, .speed = 0.01f, .tooltip = "Attachment to wheel centre at full droop (m)"})
            .field(&VehicleWheelComponent::suspension_travel, "suspension_travel", {.min = 0, .max = 10, .speed = 0.01f})
            .field(&VehicleWheelComponent::spring_frequency, "spring_frequency", {.min = 0.01f, .max = 20, .speed = 0.01f, .tooltip = "Hz"})
            .field(&VehicleWheelComponent::spring_damping, "spring_damping", {.min = 0, .max = 1, .speed = 0.01f})
            .field(&VehicleWheelComponent::steer_degrees, "steer_degrees", {.min = 0, .max = 89, .speed = 0.5f})
            .field(&VehicleWheelComponent::driven, "driven")
            .field(&VehicleWheelComponent::brake_torque, "brake_torque", {.min = 0, .max = 1e6f, .speed = 10, .tooltip = "N*m"})
            .field(&VehicleWheelComponent::handbrake_torque, "handbrake_torque", {.min = 0, .max = 1e6f, .speed = 10, .tooltip = "N*m"})
            .field(&VehicleWheelComponent::grip, "grip", {.min = 0, .max = 10, .speed = 0.01f, .tooltip = "Scales tyre friction"})
            .commit();
        scene::Schema<RagdollComponent>("Ragdoll", "Physics 3D")
            .enumeration(&RagdollComponent::mode, "mode", {"off", "kinematic", "powered", "passive"},
                         {.tooltip = "kinematic: bodies follow the animation; powered: joint motors track it; passive: limp"})
            .field(&RagdollComponent::total_mass, "total_mass", {.min = 0.1f, .max = 1e5f, .speed = 0.5f, .tooltip = "kg"})
            .field(&RagdollComponent::motor_frequency, "motor_frequency", {.min = 0.1f, .max = 30, .speed = 0.1f, .tooltip = "Joint response in Hz for the whole limb; above ~6 Hz stiff chains can chatter"})
            .field(&RagdollComponent::motor_damping, "motor_damping", {.min = 0, .max = 10, .speed = 0.01f})
            .field(&RagdollComponent::max_motor_torque, "max_motor_torque", {.min = 0, .max = 1e7f, .speed = 1, .tooltip = "N*m"})
            .field(&RagdollComponent::blend, "blend", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "0 = animation, 1 = simulation"})
            .field(&RagdollComponent::pin_root, "pin_root", {.tooltip = "powered: the root bone follows the animation"})
            .field(&RagdollComponent::gravity_compensation, "gravity_compensation",
                   {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "powered: joint torques that cancel the limbs' weight"})
            .field(&RagdollComponent::friction, "friction", {.min = 0, .max = 10, .speed = 0.01f})
            .field(&RagdollComponent::linear_damping, "linear_damping", {.min = 0, .max = 100, .speed = 0.01f})
            .field(&RagdollComponent::angular_damping, "angular_damping", {.min = 0, .max = 100, .speed = 0.01f})
            .field(&RagdollComponent::layer, "layer", {.min = 0, .max = 15})
            .field(&RagdollComponent::mask, "mask", {.hint = "bitmask16"})
            .field(&RagdollComponent::front, "front", {.tooltip = "local axis the character faces (get-up heading)"})
            .commit();
        scene::Schema<RagdollBoneComponent>("RagdollBone", "Physics 3D")
            .enumeration(&RagdollBoneComponent::shape, "shape", {"capsule", "box", "sphere"})
            .field(&RagdollBoneComponent::radius, "radius", {.min = 0.005f, .max = 100, .speed = 0.005f})
            .field(&RagdollBoneComponent::length, "length", {.min = 0, .max = 100, .speed = 0.005f, .tooltip = "0 = to the first child bone"})
            .field(&RagdollBoneComponent::mass, "mass", {.min = 0, .max = 1e5f, .speed = 0.1f, .tooltip = "kg; 0 = share of total_mass"})
            .field(&RagdollBoneComponent::swing, "swing", {.min = 0, .max = 180, .speed = 0.5f, .hint = "degrees"})
            .field(&RagdollBoneComponent::swing_plane, "swing_plane", {.min = 0, .max = 180, .speed = 0.5f, .hint = "degrees"})
            .field(&RagdollBoneComponent::twist_min, "twist_min", {.min = -180, .max = 180, .speed = 0.5f, .hint = "degrees"})
            .field(&RagdollBoneComponent::twist_max, "twist_max", {.min = -180, .max = 180, .speed = 0.5f, .hint = "degrees"})
            .field(&RagdollBoneComponent::strength, "strength", {.min = 0, .max = 10, .speed = 0.01f, .tooltip = "powered: motor torque scale"})
            .commit();
        scene::Schema<DestructibleComponent>("Destructible", "Physics 3D")
            .field(&DestructibleComponent::pieces, "pieces", {.min = 2, .max = 256, .tooltip = "Voronoi fragments"})
            .field(&DestructibleComponent::seed, "seed", {.tooltip = "Same seed, same fragments"})
            .field(&DestructibleComponent::strength, "strength", {.min = 0, .max = 1e7f, .speed = 0.5f, .tooltip = "N*s impulse that breaks it"})
            .field(&DestructibleComponent::damage_radius, "damage_radius",
                   {.min = 0, .max = 1000, .speed = 0.01f, .tooltip = "m; static: fragments freed around an impact"})
            .field(&DestructibleComponent::burst, "burst", {.min = 0, .max = 100, .speed = 0.05f, .tooltip = "m/s outward speed of freed fragments"})
            .field(&DestructibleComponent::density, "density", {.min = 1, .max = 1e5f, .speed = 1, .tooltip = "kg/m^3 (static originals)"})
            .field(&DestructibleComponent::debris_lifetime, "debris_lifetime",
                   {.min = 0, .max = 3600, .speed = 0.1f, .tooltip = "s before freed fragments are removed; 0 = keep"})
            .field(&DestructibleComponent::max_debris, "max_debris", {.min = 0, .max = 4096, .tooltip = "Freed fragments kept; oldest go first"})
            .field(&DestructibleComponent::support, "support", {.tooltip = "Static: fragments cut off from the base fall"})
            .field(&DestructibleComponent::broken, "broken")
            .commit();
        scene::Schema<DestructibleFragmentComponent>("DestructibleFragment", "Physics 3D")
            .field(&DestructibleFragmentComponent::owner, "owner", {.hint = "entity"})
            .field(&DestructibleFragmentComponent::piece, "piece")
            .field(&DestructibleFragmentComponent::age, "age")
            .commit();
        scene::Schema<ClothComponent>("Cloth", "Physics 3D")
            .field(&ClothComponent::size, "size", {.min = 0.01f, .max = 1000, .speed = 0.01f, .tooltip = "m: width (local X) and height (local Y)"})
            .field(&ClothComponent::segments_x, "segments_x", {.min = 1, .max = 128, .tooltip = "Quads across; more = finer folds, more cost"})
            .field(&ClothComponent::segments_y, "segments_y", {.min = 1, .max = 128, .tooltip = "Quads down"})
            .enumeration(&ClothComponent::pin, "pin", {"none", "top_edge", "top_corners", "left_edge", "corners"},
                         {.tooltip = "Particles held by the entity: top_edge = curtain or banner, left_edge = flag on a pole"})
            .field(&ClothComponent::mass, "mass", {.min = 0.001f, .max = 1e4f, .speed = 0.01f, .tooltip = "kg for the whole sheet"})
            .field(&ClothComponent::stretch_compliance, "stretch_compliance",
                   {.min = 0, .max = 1, .speed = 1e-5f, .tooltip = "0 = does not stretch; larger = elastic"})
            .field(&ClothComponent::shear_compliance, "shear_compliance", {.min = 0, .max = 1, .speed = 1e-5f})
            .field(&ClothComponent::bend_compliance, "bend_compliance",
                   {.min = 0, .max = 10, .speed = 1e-3f, .tooltip = "0 = stiff like card; larger = soft silk"})
            .field(&ClothComponent::damping, "damping", {.min = 0, .max = 10, .speed = 0.01f})
            .field(&ClothComponent::friction, "friction", {.min = 0, .max = 10, .speed = 0.01f})
            .field(&ClothComponent::gravity_scale, "gravity_scale", {.min = -10, .max = 10, .speed = 0.05f})
            .field(&ClothComponent::thickness, "thickness", {.min = 0, .max = 1, .speed = 0.001f, .tooltip = "m kept from colliders"})
            .field(&ClothComponent::iterations, "iterations", {.min = 1, .max = 64, .tooltip = "Solver iterations: more = stiffer, more cost"})
            .field(&ClothComponent::wind, "wind", {.speed = 0.05f, .tooltip = "m/s, world space"})
            .field(&ClothComponent::wind_drag, "wind_drag", {.min = 0, .max = 10, .speed = 0.01f})
            .field(&ClothComponent::turbulence, "turbulence", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "Gusts and flutter"})
            .field(&ClothComponent::double_sided, "double_sided", {.tooltip = "Draw the back face too"})
            .field(&ClothComponent::layer, "layer", {.min = 0, .max = 15})
            .field(&ClothComponent::mask, "mask", {.hint = "bitmask16"})
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
            .enumeration(&Collider2DComponent::shape, "shape", {"box", "circle", "capsule", "polygon", "chain"})
            .field(&Collider2DComponent::size, "size", {.min = 0.001f, .max = 1e5f, .speed = 0.01f})
            .field(&Collider2DComponent::radius, "radius", {.min = 0.001f, .max = 1e5f, .speed = 0.01f})
            .field(&Collider2DComponent::offset, "offset", {.speed = 0.01f})
            .field(&Collider2DComponent::rounding, "rounding", {.min = 0, .max = 1e4f, .speed = 0.01f, .tooltip = "Box corner radius"})
            .field(&Collider2DComponent::points, "points",
                   {.speed = 0.01f, .tooltip = "Polygon or chain outline in local space (polygon: at most 256 points)"})
            .field(&Collider2DComponent::loop, "loop", {.tooltip = "Chain: connect the last point back to the first"})
            .field(&Collider2DComponent::inside, "inside",
                   {.tooltip = "Chain loop: collide on the inside (cave and arena walls) instead of the outside"})
            .field(&Collider2DComponent::friction, "friction", {.min = 0, .max = 10, .speed = 0.01f})
            .field(&Collider2DComponent::restitution, "restitution", {.min = 0, .max = 1, .speed = 0.01f})
            .field(&Collider2DComponent::is_trigger, "is_trigger")
            .field(&Collider2DComponent::one_way, "one_way", {.tooltip = "Pass through from below, land on top (local +Y)"})
            .field(&Collider2DComponent::layer, "layer", {.min = 0, .max = 15})
            .field(&Collider2DComponent::mask, "mask", {.hint = "bitmask16"})
            .commit();
        scene::Schema<CharacterController2DComponent>("CharacterController2D", "Physics 2D")
            .field(&CharacterController2DComponent::size, "size",
                   {.min = 0.05f, .max = 100, .speed = 0.01f, .tooltip = "Capsule width x height, centred on the origin"})
            .field(&CharacterController2DComponent::max_slope, "max_slope", {.min = 1, .max = 89, .speed = 0.5f, .tooltip = "degrees"})
            .field(&CharacterController2DComponent::snap_distance, "snap_distance", {.min = 0, .max = 10, .speed = 0.01f})
            .field(&CharacterController2DComponent::gravity_scale, "gravity_scale", {.min = -10, .max = 10, .speed = 0.05f})
            .field(&CharacterController2DComponent::push_force, "push_force", {.min = 0, .max = 1e6f, .speed = 1, .tooltip = "N"})
            .field(&CharacterController2DComponent::air_control, "air_control", {.min = 0, .max = 1, .speed = 0.01f})
            .field(&CharacterController2DComponent::top_down, "top_down",
                   {.tooltip = "walk in the X/Y plane with move_character(x, y): no gravity, ground or jumps"})
            .field(&CharacterController2DComponent::layer, "layer", {.min = 0, .max = 15})
            .field(&CharacterController2DComponent::mask, "mask", {.hint = "bitmask16"})
            .commit();
        scene::Schema<Joint2DComponent>("Joint2D", "Physics 2D")
            .enumeration(&Joint2DComponent::type, "type", {"weld", "revolute", "prismatic", "distance", "wheel"})
            .field(&Joint2DComponent::connected, "connected", {.tooltip = "Other 2D body; empty = world", .hint = "entity"})
            .field(&Joint2DComponent::anchor, "anchor", {.speed = 0.01f})
            .field(&Joint2DComponent::axis, "axis", {.speed = 0.01f, .tooltip = "prismatic: slide axis; wheel: suspension axis"})
            .field(&Joint2DComponent::connected_anchor, "connected_anchor",
                   {.speed = 0.01f, .tooltip = "distance: point on the connected entity (local), or a world point"})
            .field(&Joint2DComponent::limits, "limits")
            .field(&Joint2DComponent::min_limit, "min_limit", {.speed = 0.1f, .tooltip = "revolute: degrees; others: m"})
            .field(&Joint2DComponent::max_limit, "max_limit", {.speed = 0.1f, .tooltip = "revolute: degrees; others: m"})
            .field(&Joint2DComponent::motor, "motor")
            .field(&Joint2DComponent::motor_speed, "motor_speed", {.speed = 0.1f, .tooltip = "revolute/wheel: deg/s; others: m/s"})
            .field(&Joint2DComponent::motor_force, "motor_force", {.min = 0, .max = 1e9f, .speed = 1})
            .field(&Joint2DComponent::spring, "spring")
            .field(&Joint2DComponent::spring_hertz, "spring_hertz", {.min = 0, .max = 1000, .speed = 0.05f})
            .field(&Joint2DComponent::spring_damping, "spring_damping", {.min = 0, .max = 100, .speed = 0.01f})
            .field(&Joint2DComponent::break_force, "break_force", {.min = 0, .max = 1e9f, .speed = 1, .tooltip = "0 = unbreakable"})
            .field(&Joint2DComponent::collide_connected, "collide_connected")
            .commit();
    });
}

} // namespace victus::physics
