// SPDX-License-Identifier: Apache-2.0
// Turns a cooked model's node graph into scene entities (editor drops, template characters,
// runtime spawning). Lives with the cooked format so modules above assets share one mapping.
#pragma once
#include "victus/assets/cooked.hpp"
#include "victus/scene/scene.hpp"

#include <string_view>

namespace victus::assets {

// Creates one entity per model node under `parent` (a new root named `name` when parent is
// null) with MeshRenderer components referencing (model_uuid, mesh index). Nodes with several
// primitives get one child per primitive named "<node>[<mesh index>]". Meshes of skinned nodes
// get a SkinnedMesh bound to the root; models with animations get an Animator on the root
// playing the first clip. Nodes with a light or camera get a LightComponent or CameraComponent
// (cameras are not primary). Returns the root.
[[nodiscard]] scene::LightComponent light_component(const ModelLight& light);
[[nodiscard]] scene::CameraComponent camera_component(const ModelCamera& camera);
scene::Entity instantiate_model(scene::Scene& scene, const Uuid& model_uuid, const CookedModel& model,
                                std::string_view name, scene::Entity parent = scene::kNullEntity);

} // namespace victus::assets
