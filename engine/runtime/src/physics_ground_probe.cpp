// SPDX-License-Identifier: Apache-2.0
#include "victus/runtime/physics_ground_probe.hpp"

#include "victus/physics/physics.hpp"

namespace victus::runtime {

animation::GroundProbe physics_ground_probe(const physics::PhysicsWorld& world) {
    const physics::PhysicsWorld* w = &world;
    return [w](math::Vec3 origin, math::Vec3 direction, float max_distance, std::uint32_t mask,
               scene::Entity ignore) -> std::optional<animation::GroundHit> {
        const auto hit = w->raycast(origin, direction, max_distance, mask, ignore);
        if (!hit) return std::nullopt;
        return animation::GroundHit{hit->point, hit->normal};
    };
}

} // namespace victus::runtime
