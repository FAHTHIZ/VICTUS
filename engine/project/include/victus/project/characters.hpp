// SPDX-License-Identifier: Apache-2.0
// VICTUS starter characters (docs/CHARACTERS.md): Orion and Nova, production humanoids built by
// the VICTUS character pipeline (tools/characters, ADR-0179) from the CC0 MakeHuman base mesh.
// Both share one 60-joint skeleton layout (the original 21 core joints first, in their original
// order, then twist, finger, jaw and eye joints), one clip library and one animation controller,
// so either drops into the 3D and multiplayer templates without script or collision changes.
// Their GLBs are embedded in the engine (ADR-0069, ADR-0121, ADR-0179).
#pragma once
#include "victus/core/result.hpp"
#include "victus/scene/scene.hpp"

namespace victus::assets {
struct CookedModel;
} // namespace victus::assets

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace victus::project {

struct StarterCharacter {
    std::string_view id;           // "orion", "nova" (TemplateOptions::character)
    std::string_view display_name; // "Orion"
    std::string_view description;
    std::string_view model_path; // project-relative, "assets/models/characters/victus_orion.glb"
    std::string_view uuid;       // stable asset identity written to the model's .meta
    float height = 1.70f;        // floor to top of hair in the model's own units (metres)
};

// TemplateOptions::character value for templates without a starter character.
inline constexpr std::string_view kNoCharacter = "none";
inline constexpr std::string_view kDefaultCharacter = "orion";
// Shared controller: Idle/Walk/Run blended by "speed", Jump/Fall/Land from "jump" and "grounded",
// and Interact from the "interact" trigger.
inline constexpr const char* kCharacterControllerPath = "assets/anim/characters/victus_locomotion.vanim";
inline constexpr const char* kCharacterControllerAsset = "anim/characters/victus_locomotion.vanim";
// Skeleton facts the templates and tests rely on.
// Joints [0, kCharacterCoreJointCount) are the original 21-joint humanoid (Hips ... Foot.R) in
// their original order, so scripts and retargeting maps written for it keep working.
inline constexpr std::uint32_t kCharacterJointCount = 60;
inline constexpr std::uint32_t kCharacterCoreJointCount = 21;
// Nominal height templates scale against (gameplay height / kCharacterHeight); the characters'
// own heights (StarterCharacter::height) differ, so Orion stays taller than Nova in every template.
inline constexpr float kCharacterHeight = 1.70f;

[[nodiscard]] const std::vector<StarterCharacter>& starter_characters();
// nullptr for unknown ids (including kNoCharacter).
[[nodiscard]] const StarterCharacter* find_starter_character(std::string_view id);
// The embedded GLB bytes of a character.
[[nodiscard]] std::span<const std::uint8_t> starter_character_glb(const StarterCharacter& c);
[[nodiscard]] std::string starter_character_meta(const StarterCharacter& c);
[[nodiscard]] std::string starter_character_controller();

// Creates the character's entity hierarchy under `parent` (root named `name`), exactly as
// assets::instantiate_model does for the imported model, with the Animator driven by the shared
// controller. The embedded model is parsed once and cached.
[[nodiscard]] Result<scene::Entity> instantiate_starter_character(scene::Scene& s, const StarterCharacter& c,
                                                                  std::string_view name, scene::Entity parent);

// Core joint names in skin order, socket node names and the clips the shared controller plays.
[[nodiscard]] std::span<const char* const> character_core_joint_names();
[[nodiscard]] std::span<const char* const> character_socket_names();
[[nodiscard]] std::span<const char* const> character_controller_clip_names();

// Validation of a model meant to stand in for a starter character (`victus character-check`,
// docs/CHARACTERS.md#validating-a-character). Errors break the templates or the shared controller;
// warnings are quality problems (no LODs, untextured materials, unnormalised weights).
struct CharacterIssue {
    enum class Severity : std::uint8_t { Error, Warning };
    Severity severity{Severity::Error};
    std::string code; // "skin", "joints", "sockets", "clips", "mesh", "scale", "lod", "materials", "model"
    std::string message;
};

struct CharacterValidationOptions {
    bool require_sockets = true;
    bool require_controller_clips = true;
    std::uint32_t expect_joint_count = kCharacterJointCount; // 0 = any count >= the core layout
    float min_height = 1.2f, max_height = 2.4f;             // metres, bind pose
    std::size_t lod_triangle_threshold = 20000;
};

struct CharacterReport {
    std::vector<CharacterIssue> issues;
    std::uint32_t joints = 0, lods = 0;
    std::size_t triangles = 0, clips = 0, morphs = 0, materials = 0, textures = 0, sockets = 0;
    float height = 0;
    [[nodiscard]] bool ok() const noexcept; // no errors
    [[nodiscard]] std::size_t count(CharacterIssue::Severity s) const noexcept;
};

[[nodiscard]] CharacterReport validate_character(const assets::CookedModel& model,
                                                 const CharacterValidationOptions& options = {});

} // namespace victus::project
