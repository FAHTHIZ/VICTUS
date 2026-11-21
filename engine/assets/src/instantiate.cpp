// SPDX-License-Identifier: Apache-2.0
#include "victus/assets/instantiate.hpp"

#include "victus/animation/animation.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

namespace victus::assets {

using scene::Entity;
using scene::MeshRendererComponent;
using scene::Transform;

scene::LightComponent light_component(const ModelLight& l) {
    scene::LightComponent c;
    c.type = l.type == ModelLight::Type::Directional ? scene::LightComponent::Type::Directional
             : l.type == ModelLight::Type::Spot      ? scene::LightComponent::Type::Spot
                                                     : scene::LightComponent::Type::Point;
    c.color = l.color;
    c.intensity = l.intensity;
    // Unlimited source range: where the light falls to 1% of a unit light at 1 m.
    c.range = l.range > 0.0f ? l.range : std::clamp(std::sqrt(l.intensity * 100.0f), 1.0f, 1000.0f);
    constexpr float kDeg = 57.29578f;
    c.outer_angle_degrees = l.outer_cone * kDeg;
    c.inner_angle_degrees = std::min(l.inner_cone * kDeg, c.outer_angle_degrees);
    return c;
}

scene::CameraComponent camera_component(const ModelCamera& m) {
    scene::CameraComponent c;
    c.primary = false; // an imported camera never takes over the scene's view by itself
    c.projection = m.orthographic ? scene::CameraComponent::Projection::Orthographic : scene::CameraComponent::Projection::Perspective;
    c.fov_y_degrees = m.yfov * 57.29578f;
    c.ortho_height = m.ymag * 2.0f;
    c.z_near = std::max(m.znear, 0.001f);
    c.z_far = m.zfar > c.z_near ? m.zfar : std::max(1000.0f, c.z_near * 2.0f);
    return c;
}

Entity instantiate_model(scene::Scene& scene, const Uuid& model_uuid, const CookedModel& model, std::string_view name,
                         Entity parent) {
    const Entity root = scene.create(name, parent);
    std::vector<Entity> created(model.nodes.size());
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const ModelNode& node = model.nodes[i];
        const Entity p = node.parent < 0 ? root : created[static_cast<std::size_t>(node.parent)];
        const Entity e = scene.create(node.name, p);
        Transform* t = scene.world().get<Transform>(e);
        t->position = node.translation;
        t->rotation = node.rotation;
        t->scale = node.scale;
        created[i] = e;
        if (node.light >= 0) scene.world().add<scene::LightComponent>(e, light_component(model.lights[static_cast<std::size_t>(node.light)]));
        if (node.camera >= 0) scene.world().add<scene::CameraComponent>(e, camera_component(model.cameras[static_cast<std::size_t>(node.camera)]));
        if (node.meshes.empty()) continue;
        auto add_mesh = [&](Entity target, std::uint32_t mi) {
            auto& mr = scene.world().add<MeshRendererComponent>(target);
            mr.mesh_asset = model_uuid;
            mr.mesh_index = mi;
            if (node.skin >= 0 && model.meshes[mi].data.skinned())
                scene.world().add<animation::SkinnedMeshComponent>(target, scene.uuid(root), static_cast<std::uint32_t>(node.skin));
        };
        if (node.meshes.size() == 1) {
            add_mesh(e, node.meshes[0]);
        } else {
            // Multi-primitive nodes get one child per primitive so each has its own component.
            for (const std::uint32_t mi : node.meshes) add_mesh(scene.create(std::format("{}[{}]", node.name, mi), e), mi);
        }
    }
    if (!model.animations.empty()) {
        auto& a = scene.world().add<animation::AnimatorComponent>(root);
        a.model = model_uuid;
        a.clip = model.animations.front().name;
    }
    return root;
}

} // namespace victus::assets
