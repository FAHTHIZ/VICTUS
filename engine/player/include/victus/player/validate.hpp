// SPDX-License-Identifier: Apache-2.0
// Project validation: the checks behind `victus check`, as a library the CLI, the editor's
// Problems panel and CI tools share. Each problem is a structured Diagnostic instead of a
// printed line, so callers can filter by severity or category and jump to the asset, scene or
// entity it names.
//
// The checks: project input actions, script/visual-script syntax and scene.load targets,
// materials (parent chain, textures, surface shader), surface shaders (compile), sprite sheets,
// tile sets, terrains, behaviour trees, sequences, animation controllers, and per entity in
// every prefab and scene: unknown components, scripts that do not load, missing materials,
// controllers, sprite sheets, tile sets, terrain data, behaviour trees, sequences and prefabs.
//
// Validation reads the project; opening the asset database may write `.meta` sidecars for new
// source files exactly as `victus check` always has. It does not import or cook assets.
#pragma once
#include "victus/core/fs.hpp"
#include "victus/core/result.hpp"
#include "victus/core/hash.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace victus::project {
class Project;
}

namespace victus::player {

enum class Severity : std::uint8_t { Error, Warning };

// What kind of thing a diagnostic is about. `Entity` problems come from a scene or prefab and
// carry `entity_name` / `entity_uuid`.
enum class Category : std::uint8_t {
    Project,        // victus.project (input actions)
    Script,         // .luau / .vgraph
    Material,       // .vmat
    Shader,         // surface shaders
    SpriteSheet,
    Tileset,
    Terrain,
    BehaviorTree,
    Sequence,
    AnimController, // .vanim
    Prefab,         // the prefab file itself does not load
    Scene,          // the scene file itself does not load
    Entity,         // a component on an entity in a scene or prefab
};

[[nodiscard]] std::string_view to_string(Severity s) noexcept;   // "error", "warning"
[[nodiscard]] std::string_view to_string(Category c) noexcept;   // "project", "script", ...

struct Diagnostic {
    Severity severity{Severity::Error};
    Category category{Category::Project};
    // Complete human-readable text, identical to the `victus check` line after "error: " or
    // "warning: " (it usually starts with the path).
    std::string message;
    // File the problem is in, relative to the project's assets/ folder (scripts, materials,
    // prefabs, ...) or to the project root (scenes, "victus.project"). Empty if unknown.
    std::string path;
    std::string entity_name;  // entity problems only
    Uuid entity_uuid;         // entity problems only; nil if the entity has no id
    Uuid asset;               // the asset the problem is about or references, if known
    std::string suggested_fix; // one sentence, may be empty

    // "error: <message>" / "warning: <message>" (the CLI line).
    [[nodiscard]] std::string line() const;
};

struct ValidationReport {
    std::vector<Diagnostic> diagnostics; // in check order
    std::size_t scenes{0}, prefabs{0}, materials{0}, shaders{0}, sprite_sheets{0}, tilesets{0},
        controllers{0}, scripts{0};

    [[nodiscard]] std::size_t errors() const noexcept;
    [[nodiscard]] std::size_t warnings() const noexcept;
    [[nodiscard]] bool ok() const noexcept { return errors() == 0; }
    // The summary line `victus check` prints: "checked N scenes, ...: E problems".
    [[nodiscard]] std::string summary() const;
};

// Validates an open project. Registers the engine components first (idempotent). Fails only if
// the project cannot be validated at all (asset database or script host cannot start); every
// problem in the content itself is a Diagnostic.
[[nodiscard]] Result<ValidationReport> validate_project(const project::Project& project);
// Opens the project folder (or victus.project file) and validates it.
[[nodiscard]] Result<ValidationReport> validate_project(const fs::Path& project_path);

} // namespace victus::player
