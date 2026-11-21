// SPDX-License-Identifier: Apache-2.0
// physics::make_vehicle (ADR-0166): one call that turns a box or an imported car model into a
// drivable VehicleComponent with VehicleWheel children.
#include "victus/physics/physics.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace victus::physics {

namespace {

bool wheel_name(std::string_view name) {
    std::string n(name);
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return n.find("wheel") != std::string::npos || n.find("tire") != std::string::npos ||
           n.find("tyre") != std::string::npos;
}

void configure(VehicleWheelComponent& w, bool front) {
    w.steer_degrees = front ? 30.0f : 0.0f;
    w.handbrake_torque = front ? 0.0f : 4000.0f;
    w.driven = true;
}

} // namespace

std::size_t make_vehicle(scene::Scene& scene, Entity chassis) {
    auto& ecs = scene.world();
    if (!ecs.alive(chassis)) return 0;
    if (!ecs.get<ColliderComponent>(chassis)) {
        ColliderComponent c;
        c.size = {1.8f, 0.6f, 4.0f};
        ecs.add<ColliderComponent>(chassis, c);
    }
    if (auto* rb = ecs.get<RigidBodyComponent>(chassis)) {
        rb->type = BodyType::Dynamic;
    } else {
        RigidBodyComponent r;
        r.mass = 1500;
        ecs.add<RigidBodyComponent>(chassis, r);
    }
    if (!ecs.get<VehicleComponent>(chassis)) ecs.add<VehicleComponent>(chassis);

    std::size_t wheels = 0;
    for (const Entity c : scene.children(chassis)) {
        const auto* name = ecs.get<scene::Name>(c);
        if (ecs.get<VehicleWheelComponent>(c)) {
            ++wheels;
            continue;
        }
        if (!name || !wheel_name(name->value)) continue;
        const auto* t = ecs.get<scene::Transform>(c);
        VehicleWheelComponent w;
        configure(w, t && t->position.z > 0);
        ecs.add<VehicleWheelComponent>(c, w);
        ++wheels;
    }
    if (wheels > 0) return wheels;

    // No wheels: four at the corners of the collider, half a radius inside the box's length.
    const ColliderComponent& col = *ecs.get<ColliderComponent>(chassis);
    math::Vec3 half{0.9f, 0.3f, 2.0f};
    math::Vec3 centre = col.offset;
    if (col.shape == ColliderComponent::Shape::Box) half = col.size * 0.5f;
    const float radius = std::clamp(half.y * 1.1f, 0.2f, 0.6f);
    const float width = std::clamp(half.x * 0.25f, 0.15f, 0.4f);
    const char* names[4] = {"Wheel FL", "Wheel FR", "Wheel RL", "Wheel RR"};
    for (int i = 0; i < 4; ++i) {
        const bool front = i < 2;
        const bool left = i % 2 == 0; // +X is the vehicle's left
        const Entity e = scene.create(names[i], chassis);
        ecs.get<scene::Transform>(e)->position = {centre.x + (left ? 1.0f : -1.0f) * half.x,
                                                  centre.y - half.y,
                                                  centre.z + (front ? 1.0f : -1.0f) * std::max(half.z - radius * 1.5f, 0.1f)};
        VehicleWheelComponent w;
        w.radius = radius;
        w.width = width;
        configure(w, front);
        ecs.add<VehicleWheelComponent>(e, w);
    }
    return 4;
}

} // namespace victus::physics
