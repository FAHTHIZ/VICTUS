// SPDX-License-Identifier: Apache-2.0
#include "victus/assets/instantiate.hpp"

#include "victus/animation/animation.hpp"

#include <format>
#include <vector>

namespace victus::assets {

using scene::Entity;
using scene::MeshRendererComponent;
using scene::Transform;

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
