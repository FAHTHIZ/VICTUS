// SPDX-License-Identifier: Apache-2.0
// Procedural template character: a blocky robot with an 11-joint skeleton and "Idle", "Walk", "Run"
// and "Jump" clips, written as glTF 2.0 so it goes through the normal import/cook path. The scene side
// mirrors what runtime::instantiate_model creates for the model, so template scenes can reference
// it before the asset is imported.
#pragma once
#include "victus/core/hash.hpp"
#include "victus/scene/scene.hpp"

#include <string>

namespace victus::project::robot {

// Stable asset identity written to the template's `robot.gltf.meta`.
inline constexpr const char* kModelUuid = "5d0b7e2a-91c4-4f3e-8a6d-2c7f19b04e63";
inline constexpr const char* kModelPath = "assets/models/robot.gltf";
// Animation controller (docs/ANIMATION.md) the instantiated Animator uses: Idle/Walk/Run blended by
// the "speed" parameter, plus a Jump state driven by the "jump" trigger and "grounded".
inline constexpr const char* kControllerPath = "assets/anim/robot.vanim";
inline constexpr const char* kControllerAsset = "anim/robot.vanim"; // relative to assets/

[[nodiscard]] std::string gltf();
[[nodiscard]] std::string meta();
[[nodiscard]] std::string controller();

// Creates the model's entity hierarchy under `parent` (root named `name`) with mesh renderers,
// skinned meshes and an Animator driven by kControllerAsset ("Idle" without it). Returns the root.
scene::Entity instantiate(scene::Scene& s, std::string_view name, scene::Entity parent);

} // namespace victus::project::robot
