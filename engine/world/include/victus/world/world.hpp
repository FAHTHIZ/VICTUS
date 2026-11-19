// SPDX-License-Identifier: Apache-2.0
// World streaming (ROADMAP 6h): a world is a persistent scene plus streamable cells. Each cell is
// an ordinary victus.scene file whose entities are added to the running scene when a streaming
// source comes within its load radius and removed when every source has moved beyond the radius
// plus a hysteresis margin. Cell files are read and parsed on background jobs; a parsed cell
// requests the assets it references through V.A.U.L.T. (6g) and becomes active in one step, only
// once they are resident (or after a bounded wait), so a cell is never half present.
//
// A world descriptor (`.vworld`) is JSON:
//
//     {"format": "victus.world", "version": 1, "name": "Island",
//      "load_radius": 96, "unload_margin": 16,
//      "cells": [{"id": "<uuid>", "name": "cell_0_0", "scene": "worlds/island/cell_0_0.vscene",
//                 "bounds": {"min": [x, y, z], "max": [x, y, z]}, "load_radius": 0}, ...]}
//
// Cell paths are relative to the project root. Cell identity is the uuid, stable across edits.
// Entities keep their uuids; the state of an unloaded cell (moved, destroyed or changed
// entities) is kept and restored on reload, and can be written into a save game.
#pragma once
#include "victus/core/fs.hpp"
#include "victus/core/hash.hpp"
#include "victus/core/jobs.hpp"
#include "victus/core/json.hpp"
#include "victus/core/math.hpp"
#include "victus/scene/scene.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace victus::world {

inline constexpr int kWorldFormatVersion = 1;
// The versioned victus.world format and its migration chain (ADR-0100).
[[nodiscard]] const migration::Format& world_format();
inline constexpr std::string_view kWorldExtension = ".vworld";

struct CellDesc {
    Uuid id;
    std::string name;
    std::string scene; // project-relative path of the cell's victus.scene file
    math::Aabb bounds; // world-space extent of the cell's content
    float load_radius{0.0f}; // 0 = the world's load_radius
};

struct WorldDesc {
    std::string name{"World"};
    float load_radius{96.0f};   // cells within this distance of a source load
    float unload_margin{16.0f}; // and unload beyond load_radius + unload_margin (hysteresis)
    std::vector<CellDesc> cells;
};

// Validates format/version, unique non-nil cell ids, non-empty scene paths, valid bounds and
// non-negative radii.
[[nodiscard]] Result<WorldDesc> parse_world(const json::Value& doc);
[[nodiscard]] json::Value world_to_json(const WorldDesc& world);
[[nodiscard]] Result<WorldDesc> load_world_file(const fs::Path& path);
[[nodiscard]] Status save_world_file(const WorldDesc& world, const fs::Path& path);

// --- Components -------------------------------------------------------------------------------

// Loads cells around this entity (Unreal's streaming-source component; the player's camera is
// the fallback source when a scene has none). radius_scale multiplies every load radius.
struct StreamingSourceComponent {
    float radius_scale{1.0f};
    bool enabled{true};
};

// On an entity of the persistent scene: the world (`.vworld`, project-relative) the player streams.
struct WorldStreamingComponent {
    std::string world;
};

// Added (not serialized) to every entity a cell created; `cell` is the owning cell's id.
struct CellMember {
    Uuid cell;
};

void register_components();

// --- Streaming --------------------------------------------------------------------------------

struct StreamingSource {
    math::Vec3 position;
    float radius_scale{1.0f};
};

// Enabled StreamingSourceComponent entities, else `fallback` when given.
[[nodiscard]] std::vector<StreamingSource> gather_sources(scene::Scene& scene, const math::Vec3* fallback = nullptr);

// How cells reach V.A.U.L.T.: `request` starts loading an asset uuid referenced by a cell (it is
// called for every uuid-shaped string in the cell's components, so it must ignore unknown ids),
// and `ready` reports whether it is resident, failed or unknown (anything that will never block).
struct Dependencies {
    std::function<void(const Uuid&)> request;
    std::function<bool(const Uuid&)> ready;
};

enum class CellState : std::uint8_t { Unloaded, Loading, Waiting, Active, Failed };
[[nodiscard]] std::string_view cell_state_name(CellState s) noexcept;

struct StreamerSettings {
    std::uint32_t max_loading{2};             // cell files read/parsed at once
    std::uint32_t activations_per_update{1};  // cells added to the scene per update()
    std::uint32_t max_wait_updates{600};      // a cell waits at most this long for its assets
};

struct StreamerStats {
    std::uint32_t active{0}, loading{0}, waiting{0}, failed{0};
    std::uint32_t active_entities{0};
    std::uint64_t loads{0}, unloads{0}, cancelled{0}, dependency_timeouts{0};
    std::uint64_t updates{0};
};

class WorldStreamer {
public:
    // `root` resolves cell paths. `jobs` null = cells load synchronously inside update().
    WorldStreamer(WorldDesc world, fs::Path root, jobs::JobSystem* jobs = nullptr, Dependencies deps = {},
                  StreamerSettings settings = {});
    ~WorldStreamer();
    WorldStreamer(const WorldStreamer&) = delete;
    WorldStreamer& operator=(const WorldStreamer&) = delete;

    // Decides which cells each source wants, starts/cancels loads (nearest first), activates
    // parsed cells whose assets are ready and removes cells no source wants (keeping their state).
    void update(scene::Scene& scene, std::span<const StreamingSource> sources);
    // Removes every active cell from the scene (their state is kept) and cancels loads.
    void unload_all(scene::Scene& scene);
    // Blocks until running cell reads finish (tests, shutdown).
    void wait_idle();

    [[nodiscard]] CellState state(const Uuid& cell) const;
    [[nodiscard]] const Error* error(const Uuid& cell) const;
    [[nodiscard]] const WorldDesc& world() const noexcept { return world_; }
    [[nodiscard]] StreamerStats stats() const;

    // Saved state of cells that have been unloaded at least once, plus the current state of
    // active cells (snapshots of their entities). load_state() must run before cells activate.
    [[nodiscard]] json::Value save_state(const scene::Scene& scene) const;
    [[nodiscard]] Status load_state(const json::Value& state);

private:
    struct Job;
    struct Cell {
        const CellDesc* desc{nullptr};
        CellState state{CellState::Unloaded};
        std::shared_ptr<Job> job;     // Loading
        json::Value doc;              // Waiting: the parsed cell (file or saved state)
        std::vector<Uuid> deps;       // Waiting: asset uuids it references
        std::uint32_t waited{0};
        std::vector<Uuid> roots;      // Active: uuids of the cell's root entities
        std::uint32_t entities{0};
        std::optional<json::Value> saved; // state captured at the last unload
        Error error;
    };
    void start_load(Cell& cell);
    void collect(Cell& cell);
    void activate(scene::Scene& scene, Cell& cell);
    void deactivate(scene::Scene& scene, Cell& cell);
    [[nodiscard]] json::Value snapshot(const scene::Scene& scene, const Cell& cell) const;

    WorldDesc world_;
    fs::Path root_;
    jobs::JobSystem* jobs_;
    Dependencies deps_;
    StreamerSettings settings_;
    std::unordered_map<Uuid, Cell, UuidHash> cells_;
    jobs::Counter counter_;
    StreamerStats totals_;
};

// Reads and parses a cell document and lists the uuid-shaped strings in its components that are
// not entities of the cell itself (candidate asset references).
[[nodiscard]] Result<json::Value> read_cell(const fs::Path& path);
[[nodiscard]] std::vector<Uuid> referenced_uuids(const json::Value& cell_doc);

// --- Authoring --------------------------------------------------------------------------------

struct PartitionSettings {
    float cell_size{64.0f}; // XZ grid
    std::string directory;  // project-relative folder for cell files, e.g. "worlds/island"
    std::string name{"World"};
    float load_radius{0.0f}; // 0 = 1.5 x cell_size
};

struct Partition {
    WorldDesc world;
    std::vector<std::pair<std::string, json::Value>> cells; // (project-relative path, victus.scene doc)
    json::Value persistent;                                 // the scene with the cell content removed
    std::size_t moved_entities{0};
};

// Splits `scene`'s root subtrees into XZ grid cells by the root's world position. Subtrees that
// hold a camera, streaming source, world-streaming marker, environment, directional light,
// audio listener, script, terrain, tilemap, UI canvas, character controller or network
// identity stay persistent, as do subtrees whose root is scaled wider than a cell (ground
// planes). The persistent scene gains a WorldStreaming entity pointing at `world_path`.
[[nodiscard]] Result<Partition> partition_scene(const scene::Scene& scene, const PartitionSettings& settings,
                                                const std::string& world_path);

} // namespace victus::world
