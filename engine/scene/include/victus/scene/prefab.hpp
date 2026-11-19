// SPDX-License-Identifier: Apache-2.0
// Prefabs: reusable entity trees saved as `.vprefab` assets and instantiated into scenes.
//
// A prefab file is JSON in the scene snapshot format:
//
//     {"format": "victus.prefab", "version": 1, "name": "Coin",
//      "entities": [ {uuid, name, parent?, next?, components}, ... ]}   // root first, parents first
//
// The uuids inside a prefab are local to it. An instance is an ordinary entity subtree:
//   - its root carries `PrefabInstance` (the prefab asset's UUID and the instance's overrides);
//   - every other entity that came from the prefab carries `PrefabPart` (the local uuid it came from).
// Instance entity uuids are derived from the instance root's uuid and the local uuid, so the same
// prefab instantiated with the same root uuid always produces the same uuids (stable references,
// network spawning, deterministic tests), and entity references between entities of the prefab
// (joints, script properties holding uuids) are remapped to the instance.
//
// Updating an instance from a changed prefab keeps the root's name, Transform, parent and sibling
// position, every component listed in its overrides, and entities the user added under the
// instance; parts the prefab no longer contains are removed. Nested prefab instances inside a
// prefab are flattened when the prefab is created (they become ordinary parts).
#pragma once
#include "victus/scene/scene.hpp"

#include <optional>

namespace victus::scene {

inline constexpr int kPrefabFormatVersion = 1;
// The versioned victus.prefab format and its migration chain (ADR-0100).
[[nodiscard]] const migration::Format& prefab_format();
inline constexpr std::string_view kPrefabExtension = ".vprefab";

struct PrefabOverride {
    Uuid part;             // local uuid of the prefab entity (the prefab root's uuid for the root)
    std::string component; // serializer name, or "name" for the entity name
    friend bool operator==(const PrefabOverride&, const PrefabOverride&) = default;
};

struct PrefabInstance {
    Uuid prefab; // asset UUID of the .vprefab
    std::vector<PrefabOverride> overrides;
};

struct PrefabPart {
    Uuid source; // local uuid inside the prefab
};

struct Prefab {
    std::string name{"Prefab"};
    json::Value entities = json::Value::array(); // snapshots, root first, parents before children
    std::vector<std::string> migrations;         // what parse_prefab upgraded (ADR-0100); empty when current

    [[nodiscard]] Uuid root() const;
    [[nodiscard]] std::size_t size() const noexcept { return entities.size(); }
};

// Parses and validates a prefab document: format/version, unique non-nil uuids, exactly one root
// (first entity), every parent defined earlier, components an object.
[[nodiscard]] Result<Prefab> parse_prefab(const json::Value& doc);
[[nodiscard]] Result<Prefab> load_prefab_file(const std::filesystem::path& path);
[[nodiscard]] json::Value prefab_to_json(const Prefab& prefab);
[[nodiscard]] Status save_prefab_file(const Prefab& prefab, const std::filesystem::path& path);

// Builds a prefab from `root` and its descendants. Entities that are parts of an instance keep
// their local uuid when they belong to `root`'s own instance (so "apply" keeps identities);
// other entities use their current uuid. The root's position is stored as zero (an instance's
// placement belongs to the instance); rotation and scale are kept. PrefabInstance/PrefabPart
// components are stripped (nested instances are flattened).
[[nodiscard]] Prefab make_prefab(const Scene& scene, Entity root);

// Turns `root`'s subtree into an instance of the prefab created from it with make_prefab (adds
// PrefabInstance to the root and PrefabPart to its descendants; uuids are unchanged).
void link_as_instance(Scene& scene, Entity root, const Uuid& prefab_asset);

struct InstantiateOptions {
    Entity parent;                       // null = scene root
    std::optional<math::Vec3> position;  // local position of the instance root (else the prefab's)
    Uuid uuid;                           // instance root uuid; nil = random
    std::string name;                    // empty = the prefab root's name
};
// Creates an instance of `prefab` (whose asset UUID is `prefab_asset`) and returns its root.
[[nodiscard]] Result<Entity> instantiate(Scene& scene, const Prefab& prefab, const Uuid& prefab_asset,
                                         const InstantiateOptions& options = {});

// Uuid of the instance entity created from local uuid `part` under an instance root with `root`.
[[nodiscard]] Uuid derive_part_uuid(const Uuid& root, const Uuid& part) noexcept;

// The instance root `e` belongs to (itself when it is a root; the nearest ancestor with
// PrefabInstance otherwise), or null when `e` is not part of an instance.
[[nodiscard]] Entity instance_root(const Scene& scene, Entity e);
// Local uuid of `e` within its instance (root -> prefab root uuid given the prefab), nil if none.
[[nodiscard]] Uuid part_source(const Scene& scene, Entity e, const Prefab& prefab);

// Records that `component` (serializer name or "name") of `e` differs from the prefab on purpose.
// No-op when `e` is not part of an instance. Returns true when an override was added.
bool mark_override(Scene& scene, Entity e, std::string_view component, const Prefab& prefab);
[[nodiscard]] bool is_overridden(const Scene& scene, Entity e, std::string_view component, const Prefab& prefab);

// Brings the instance rooted at `root` in line with `prefab` (see the header comment). With
// `keep_overrides` false every override is dropped first ("revert").
[[nodiscard]] Status update_instance(Scene& scene, Entity root, const Prefab& prefab, bool keep_overrides = true);
// Updates every instance of `prefab_asset` in the scene; returns how many were updated.
[[nodiscard]] Result<std::size_t> update_instances(Scene& scene, const Uuid& prefab_asset, const Prefab& prefab);
// Every instance root in the scene (optionally only those of one prefab), in hierarchy order.
[[nodiscard]] std::vector<Entity> find_instances(const Scene& scene, const Uuid& prefab_asset = {});

// Creates the prefab content from the instance rooted at `root` ("apply"): parts keep their local
// uuids, entities added under the instance become new parts, the prefab root keeps `previous`'s
// root Transform. The instance's overrides are cleared and the added entities are linked.
[[nodiscard]] Prefab apply_instance(Scene& scene, Entity root, const Prefab& previous);

namespace detail {
void register_prefab_serializers(SerializerRegistry& registry);
}

} // namespace victus::scene
