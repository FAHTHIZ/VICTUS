// SPDX-License-Identifier: Apache-2.0
// Skeleton inspection, safe extension and versioned migration of cooked models (ADR-0205).
//
// A model's skeleton is its node graph plus one or more skins (joint lists with inverse bind
// matrices). Everything else refers to it: skin streams by joint index, clips by node index,
// scenes, sockets, retarget maps, ragdolls, IK and scripts by bone *name*. This module edits the
// skeleton so that those references survive:
//
// * Adding a bone appends a node (and, for deforming bones, a skin joint) at the end, so every
//   existing node index, joint index, clip channel and name stays valid. Optional weight
//   distribution moves part of a parent's weights to the new bone (twist and corrective joints).
// * Renaming keeps the node index (clips, skins) and records the old name as an alias.
// * Reparenting keeps the bone's rest world transform and re-bakes clips that animate it so the
//   animated world motion is unchanged.
// * Removing a bone hands its weights to a surviving joint (its nearest weighted ancestor by
//   default), reparents its children (keeping their world transforms) and remaps every index.
//
// A migration (.vskelmig JSON) is a list of edits plus the skeleton layout hash it applies to and
// the version it produces; a model whose skeleton does not match is refused rather than edited
// blindly. Migrations run at import (ModelImportSettings::skeleton_migrations) so source files are
// never modified, and check_skeleton_compatibility() reports what an edit broke for dependants
// (missing names, changed parents, moved sockets, lost clip channels).
#pragma once
#include "victus/assets/cooked.hpp"
#include "victus/core/fs.hpp"
#include "victus/core/json.hpp"
#include "victus/core/result.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace victus::assets {

// ---- roles -------------------------------------------------------------------------------------

// What a bone is for, inferred from its name with the conventions of VICTUS, Blender/Rigify,
// Mixamo, Unreal and MakeHuman. Names are the persistent record of a role (cooked models carry no
// separate role table), so VICTUS rigs name helpers explicitly: "IK.", "Pole.", "Socket.",
// "Mech.", "Dyn." (secondary motion), "Fix." (corrective). A bone can have several roles.
enum BoneRole : std::uint32_t {
    kBoneRoot = 1u << 0,
    kBoneSpine = 1u << 1,
    kBoneNeck = 1u << 2,
    kBoneHead = 1u << 3,
    kBoneArm = 1u << 4,
    kBoneHand = 1u << 5,
    kBoneFinger = 1u << 6,
    kBoneLeg = 1u << 7,
    kBoneFoot = 1u << 8,
    kBoneToe = 1u << 9,
    kBoneTwist = 1u << 10,
    kBoneFace = 1u << 11,
    kBoneEye = 1u << 12,
    kBoneJaw = 1u << 13,
    kBoneTongue = 1u << 14,
    kBoneSecondary = 1u << 15,  // jiggle, cloth, hair, antenna, cable: driven by simulation
    kBoneMechanical = 1u << 16, // pistons, hinges, wheels, turrets: rigid articulation
    kBoneCorrective = 1u << 17, // helper joints that keep volume (elbow, knee, shoulder)
    kBoneIkTarget = 1u << 18,
    kBoneIkPole = 1u << 19,
    kBoneSocket = 1u << 20,     // attachment/equipment point
    kBoneTail = 1u << 21,
    kBoneWing = 1u << 22,
    kBoneEar = 1u << 23,
};
[[nodiscard]] std::uint32_t infer_bone_roles(std::string_view name) noexcept;
// "arm|twist" style list (stable order), "" for none.
[[nodiscard]] std::string bone_roles_string(std::uint32_t roles);
// Parses bone_roles_string's output (unknown names are ignored).
[[nodiscard]] std::uint32_t parse_bone_roles(std::string_view text) noexcept;

// ---- inspection --------------------------------------------------------------------------------

struct SkeletonBone {
    std::string name;
    std::int32_t node{-1};
    std::int32_t parent_node{-1};
    std::int32_t joint{-1};        // index in the skin (-1: helper node, socket or not deforming)
    std::uint32_t roles{0};
    std::uint32_t depth{0};
    std::uint32_t children{0};
    std::uint32_t weighted_vertices{0}; // across every mesh bound to the skin
    math::Vec3 rest_position;           // model space, bind pose
    float length{0};                    // distance to its first child (0 for leaves)
    bool animated{false};               // some clip has a channel on it
};

struct SkeletonInfo {
    std::uint32_t skin{0};
    std::vector<SkeletonBone> bones; // the skin's joints and every node below the skeleton root(s)
    std::uint32_t joints{0}, helpers{0}, sockets{0}, max_depth{0}, unweighted_joints{0};
    std::uint64_t layout_hash{0};    // skeleton_layout_hash()
    [[nodiscard]] const SkeletonBone* find(std::string_view name) const noexcept;
    [[nodiscard]] std::uint32_t count(std::uint32_t role) const noexcept;
};
[[nodiscard]] SkeletonInfo describe_skeleton(const CookedModel& model, std::uint32_t skin = 0);
// Hash of the skin's joint names and their parents' names in joint order: equal hashes mean
// skin streams, ragdoll maps and retarget maps written for one model fit the other.
[[nodiscard]] std::uint64_t skeleton_layout_hash(const CookedModel& model, std::uint32_t skin = 0);

// Model-space bind transform of every node (skin inverse binds win for joints, the rest pose
// chain is used for the others).
[[nodiscard]] std::vector<math::Mat4> bind_transforms(const CookedModel& model, std::uint32_t skin = 0);

// Model-space transforms of every node posed by `clip` at `time` (rest pose where it has no
// channel; null = rest pose), and the skin palette (joint transform * inverse bind) of a pose:
// what the GPU receives, for CPU skinning, diagnostics and tests.
[[nodiscard]] std::vector<math::Mat4> pose_world(const CookedModel& model, const animation::Clip* clip, float time);
// `overrides` (node, local rotation) replace the posed local rotations first (Studio pose editing).
[[nodiscard]] std::vector<math::Mat4> pose_world(const CookedModel& model, const animation::Clip* clip, float time,
                                                 std::span<const std::pair<std::uint32_t, math::Quat>> overrides);
[[nodiscard]] std::vector<math::Mat4> pose_palette(const CookedModel& model, std::span<const math::Mat4> world,
                                                   std::uint32_t skin = 0);

// ---- editing -----------------------------------------------------------------------------------

struct BoneEdit {
    // Move (ADR-0208): sets the rest transform (relative to its parent) of a node that does not
    // deform and has no deforming descendant: sockets, IK targets, poles, helpers. Joints are
    // refused because moving one changes its bind and every pose of the mesh.
    enum class Op : std::uint8_t { Add, Rename, Reparent, Remove, Move };
    Op op{Op::Add};
    std::string bone;   // Add: new name; others: existing bone
    std::string parent; // Add / Reparent: parent bone (must exist)
    std::string name;   // Rename: new name; Remove: joint receiving the weights ("" = nearest weighted ancestor)
    // Add / Move: rest transform relative to the parent.
    math::Vec3 translation{};
    math::Quat rotation{};
    math::Vec3 scale{1, 1, 1};
    bool deform{true};      // Add: becomes a skin joint (false: helper, IK target or socket node)
    // Add (deforming): move part of `weights_from`'s weights to the new bone. For a vertex at
    // parameter t along the source bone (0 at its head, 1 at its first child), the moved share is
    // `weight_share` * smoothstep(weight_start, weight_end, t). Twist joints use 0.5 / 0..1.
    std::string weights_from;
    float weight_share{0.0f};
    float weight_start{0.0f}, weight_end{1.0f};
    bool force{false};      // Rename/Remove: allowed on protected (starter core) joints
};

struct SkeletonEditReport {
    std::vector<std::string> added, removed;
    std::vector<std::pair<std::string, std::string>> renamed; // (old, new)
    std::vector<std::string> reparented;
    std::vector<std::string> moved;
    std::vector<std::int32_t> node_remap;  // old node index -> new (-1 removed); identity when no removal
    std::vector<std::int32_t> joint_remap; // old joint index -> new (-1 removed)
    std::size_t vertices_reweighted{0};
    std::size_t vertices_full{0}; // Add: vertices left alone because all four influence slots were taken
    std::size_t channels_rebaked{0}, channels_dropped{0};
    std::vector<std::string> warnings;
    std::uint64_t layout_before{0}, layout_after{0};
};

struct SkeletonEditOptions {
    std::uint32_t skin{0};
    // Names that Rename/Remove refuse without BoneEdit::force (starter core joints, sockets).
    std::vector<std::string> protected_bones;
    std::uint32_t max_bake_samples{4096}; // per clip when re-baking reparented bones
    float bake_rate{30.0f};               // samples per second added between keys when re-baking
    // Remove: false keeps the removed joint's skin slot (pointing at the joint that received its
    // weights, with that joint's inverse bind) so every other joint keeps its index; true erases
    // the slot and renumbers the joints after it.
    bool compact_joints{false};
};

// Applies `edits` in order. On error the model is left unchanged (edits run on a copy).
[[nodiscard]] Result<SkeletonEditReport> apply_skeleton_edits(CookedModel& model, std::span<const BoneEdit> edits,
                                                              const SkeletonEditOptions& options = {});

// ---- migrations --------------------------------------------------------------------------------

struct SkeletonMigration {
    std::string profile;          // e.g. "VictusHumanoid"
    std::uint32_t from_version{1};
    std::uint32_t to_version{2};
    std::uint64_t requires_layout{0}; // skeleton_layout_hash before; 0 = any
    std::string description;
    std::vector<BoneEdit> edits;
    [[nodiscard]] json::Value to_json() const;
    static Result<SkeletonMigration> from_json(const json::Value& v);
};
[[nodiscard]] Result<SkeletonMigration> parse_skeleton_migration(std::string_view text);
[[nodiscard]] std::string write_skeleton_migration(const SkeletonMigration& migration);

// Checks the layout hash, then applies the edits.
[[nodiscard]] Result<SkeletonEditReport> apply_skeleton_migration(CookedModel& model, const SkeletonMigration& migration,
                                                                  const SkeletonEditOptions& options = {});

struct ModelImportSettings;
// Import hook: reads `settings.skeleton_migrations` (relative to `model_path`'s directory, inside
// `sandbox_root`), applies them in order and appends the files to `dependencies` so editing a
// migration re-imports the model. Errors name the migration file.
[[nodiscard]] Result<void> apply_import_skeleton_migrations(CookedModel& model, const ModelImportSettings& settings,
                                                            const fs::Path& model_path, const fs::Path& sandbox_root,
                                                            std::vector<fs::Path>& dependencies);

// ---- compatibility -----------------------------------------------------------------------------

struct SkeletonCompatibilityIssue {
    enum class Severity : std::uint8_t { Error, Warning, Info };
    Severity severity{Severity::Error};
    std::string bone;
    std::string message;
};
struct SkeletonCompatibility {
    std::vector<SkeletonCompatibilityIssue> issues;
    std::size_t preserved_bones{0}, added_bones{0}, missing_bones{0};
    [[nodiscard]] bool ok() const noexcept; // no errors
};
struct CompatibilityOptions {
    std::uint32_t skin{0};
    float socket_tolerance{1e-3f};     // metres a socket may move in the bind pose
    float rest_tolerance{1e-3f};       // metres an existing joint may move in the bind pose
    // Old -> new names that count as preserved (a migration's renames).
    std::vector<std::pair<std::string, std::string>> renames;
};
// Compares `after` with `before`: every bone of `before` must exist in `after` (or be renamed),
// keep its parent and bind position, existing joints must keep their skin index (so other assets'
// joint indices stay valid), sockets must not move and clips must keep their channels.
[[nodiscard]] SkeletonCompatibility check_skeleton_compatibility(const CookedModel& before, const CookedModel& after,
                                                                 const CompatibilityOptions& options = {});

// ---- automatic weights -------------------------------------------------------------------------

// Rebuilds skin weights of every mesh bound to `skin` from the distance of each vertex to the
// deforming bones' segments (head to first child, or a short segment along the bone for
// leaves): w = 1 / (d + eps)^falloff over the nearest bones, top four, normalised. A procedural
// stand-in for Blender's "automatic weights" for generated rigs and new bones; bones listed in
// `only` (empty = all) are the only ones that receive weight.
struct AutoWeightSettings {
    float falloff{4.0f};
    float epsilon{0.01f};
    std::vector<std::string> only;
    std::uint32_t skin{0};
};
std::size_t auto_skin_weights(CookedModel& model, const AutoWeightSettings& settings = {});

} // namespace victus::assets
