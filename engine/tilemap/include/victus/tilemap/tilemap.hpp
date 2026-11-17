// SPDX-License-Identifier: Apache-2.0
// 2D tilemaps: tile sets (.vtileset) slice a texture into a grid of tiles and describe their
// collision and autotiling rules; a Tilemap component stores painted cells and is drawn through
// the sprite batcher. Solid tiles become merged static Box2D boxes.
//
// .vtileset (JSON):
//   {
//     "format": "victus.tileset", "version": 1,
//     "texture": "<uuid of the tile texture>",
//     "columns": 8, "rows": 4,                   // grid, tiles numbered row by row from top-left
//     "collision": "none",                        // default for tiles not listed below
//     "tiles": { "0": { "collision": "solid" }, "9": { "collision": "one_way" } },
//     "terrains": {                               // autotiling: 16 tiles per terrain indexed by the
//       "ground": [ 0, 1, 2, ... 15 ]             // 4-bit mask of same-terrain neighbours
//     }                                           // (north = 1, east = 2, south = 4, west = 8)
//   }
//
// Cells: tile (x, y) covers the local rectangle [x, x+1) * cell_size.x by [y, y+1) * cell_size.y;
// +Y is up (north). The entity's transform places, rotates and scales the whole map.
#pragma once

#include "victus/core/hash.hpp"
#include "victus/core/json.hpp"
#include "victus/core/math.hpp"
#include "victus/core/result.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/scene/scene.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace victus::tilemap {

inline constexpr std::string_view kTilesetExtension = ".vtileset";
inline constexpr std::int64_t kTilesetVersion = 1;
inline constexpr std::uint32_t kMaxTiles = 65536;
// Painting further than this from the origin is rejected (keeps keys and float positions exact).
inline constexpr std::int32_t kMaxCoordinate = 1 << 20;

enum class TileCollision : std::uint8_t { None, Solid, OneWay };
[[nodiscard]] const char* to_string(TileCollision c) noexcept;
[[nodiscard]] std::optional<TileCollision> parse_collision(std::string_view s) noexcept;

struct Terrain {
    std::string name;
    std::array<std::int32_t, 16> tiles{}; // tile for each neighbour mask (N=1, E=2, S=4, W=8)
};

struct Tileset {
    Uuid texture;
    std::uint32_t columns{1}, rows{1};
    TileCollision default_collision{TileCollision::None};
    std::unordered_map<std::uint32_t, TileCollision> collision; // per-tile overrides
    std::vector<Terrain> terrains;

    [[nodiscard]] std::uint32_t tile_count() const noexcept { return columns * rows; }
    // u0, v0 (top), u1, v1 (bottom) of a tile, as render::Sprite::uv.
    [[nodiscard]] math::Vec4 tile_uv(std::uint32_t tile) const noexcept;
    [[nodiscard]] TileCollision collision_of(std::int32_t tile) const noexcept;
    [[nodiscard]] std::int32_t terrain_index(std::string_view name) const noexcept; // -1 when missing
};

[[nodiscard]] Result<Tileset> parse_tileset(const json::Value& doc);
[[nodiscard]] json::Value to_json(const Tileset& tileset);
[[nodiscard]] Result<Tileset> load_tileset_file(const std::filesystem::path& path);
[[nodiscard]] Status save_tileset_file(const std::filesystem::path& path, const Tileset& tileset);

struct Cell {
    std::int32_t tile{-1};     // tile index in the tileset; < 0 = empty
    std::int16_t terrain{-1};  // terrain the cell was painted with (autotiling), -1 = none
    bool flip_x{false};
    bool flip_y{false};
    friend bool operator==(const Cell&, const Cell&) = default;
};

[[nodiscard]] constexpr std::uint64_t cell_key(std::int32_t x, std::int32_t y) noexcept {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(y);
}
[[nodiscard]] constexpr std::int32_t key_x(std::uint64_t k) noexcept { return static_cast<std::int32_t>(static_cast<std::uint32_t>(k >> 32)); }
[[nodiscard]] constexpr std::int32_t key_y(std::uint64_t k) noexcept { return static_cast<std::int32_t>(static_cast<std::uint32_t>(k)); }

struct TilemapComponent {
    Uuid tileset;                    // .vtileset asset
    math::Vec2 cell_size{1, 1};      // world units per cell
    math::Color tint{1, 1, 1, 1};
    std::int32_t layer{0};           // sprite sorting layer
    bool collision{true};            // build static 2D colliders from solid/one-way tiles
    float friction{0.6f};
    std::uint32_t collision_layer{0};
    std::uint32_t collision_mask{0xFFFFu};
    // Painted cells (saved in the scene as "cells": [[x, y, tile, flags?, terrain?], ...]).
    std::unordered_map<std::uint64_t, Cell> cells;
    std::uint64_t revision{0};       // bumped on every cell change (runtime, not saved)

    [[nodiscard]] const Cell* cell(std::int32_t x, std::int32_t y) const noexcept;
    [[nodiscard]] std::int32_t tile(std::int32_t x, std::int32_t y) const noexcept; // -1 when empty
};

// Registers "Tilemap" (reflected fields plus the cell list). Idempotent.
void register_components();

// ---- editing (all return false for coordinates beyond kMaxCoordinate) -----------------------
// tile < 0 erases. Clears the cell's terrain.
bool set_tile(TilemapComponent& map, std::int32_t x, std::int32_t y, std::int32_t tile, bool flip_x = false,
              bool flip_y = false);
// Fills the inclusive rectangle; returns the number of cells changed (0 when out of range or
// larger than kMaxFillCells).
inline constexpr std::uint64_t kMaxFillCells = 1u << 20;
std::uint64_t fill_tiles(TilemapComponent& map, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                         std::int32_t tile);
void clear_tiles(TilemapComponent& map);
// Paints (terrain >= 0) or erases (terrain < 0) a terrain cell and re-picks the tiles of it and
// its four neighbours from the tileset's 16-tile rule. False for an unknown terrain index.
bool paint_terrain(TilemapComponent& map, const Tileset& tileset, std::int32_t x, std::int32_t y, std::int32_t terrain);
// Recomputes every terrain cell (after the tileset's rules changed).
void refresh_terrain(TilemapComponent& map, const Tileset& tileset);

struct TileBounds {
    std::int32_t min_x{0}, min_y{0}, max_x{-1}, max_y{-1}; // inclusive; empty when max < min
    [[nodiscard]] bool empty() const noexcept { return max_x < min_x || max_y < min_y; }
};
[[nodiscard]] TileBounds bounds(const TilemapComponent& map) noexcept;

// Local-space collision rectangles (cell units already multiplied by cell_size): runs of
// solid/one-way cells merged greedily into rows, then rows of equal span merged upwards.
struct TileBox {
    math::Vec2 min, max;
    TileCollision type{TileCollision::Solid};
};
[[nodiscard]] std::vector<TileBox> collision_boxes(const TilemapComponent& map, const Tileset& tileset);

// ---- runtime --------------------------------------------------------------------------------

using TilesetProvider = std::function<std::shared_ptr<const Tileset>(const Uuid& tileset)>;

// Written by TilemapSystem on every Tilemap entity (not saved): the resolved tileset.
struct TilemapRuntime {
    std::shared_ptr<const Tileset> tileset;
    Uuid resolved;                  // tileset uuid `tileset` was resolved for
    std::uint64_t built_revision{~0ull};
    const void* built_tileset{nullptr};
    bool built_collision{false};
    std::uint32_t boxes{0};
};

struct TilemapStats {
    std::uint32_t maps{0}, missing_tilesets{0}, collision_rebuilds{0}, boxes{0};
};

// Resolves tilesets and keeps each map's static colliders (physics::StaticShapes2DComponent)
// in step with its cells.
class TilemapSystem {
public:
    explicit TilemapSystem(TilesetProvider provider = {});
    void add_tileset(const Uuid& id, std::shared_ptr<const Tileset> tileset); // takes precedence
    void clear_cache();  // after .vtileset files changed
    [[nodiscard]] std::shared_ptr<const Tileset> tileset(const Uuid& id);
    void update(scene::Scene& scene);
    [[nodiscard]] const TilemapStats& stats() const noexcept { return stats_; }

private:
    TilesetProvider provider_;
    std::unordered_map<Uuid, std::shared_ptr<const Tileset>, UuidHash> added_, cache_;
    TilemapStats stats_;
    std::uint64_t generation_{0};
};

// Runs update() in PreUpdate (before physics steps) and PostUpdate (after scripts painted).
void add_tilemap_systems(gameplay::Simulation& sim, TilemapSystem& system);

} // namespace victus::tilemap
