// SPDX-License-Identifier: Apache-2.0
// Procedural city (ADR-0202, GAP-20): one deterministic pass from a seed to a road network, city
// blocks, lots, building massing, street furniture and pedestrian paths, then to renderable
// per-cell meshes, a traffic lane graph (ADR-0167) and crowd pedestrians (ADR-0200).
//
// Layout: an irregular grid. Column and row spacings vary per seed; every `avenue_every`-th road
// is a wider avenue. Each block is a raised kerb plate (sidewalk ring plus interior), split into
// lots along its frontage: two back-to-back rows when the block is shallow, a perimeter ring of
// lots around a courtyard when it is deep. Lots get a building whose height falls off with the
// distance from the downtown centre; tall buildings step back in tiers. Some blocks are parks.
// Street lights and trees line the sidewalks; pedestrians walk loops around the blocks.
//
// Conventions: Y up, metres, the city centred on the origin of its owner entity, ground at y = 0.
#pragma once
#include "victus/core/math.hpp"
#include "victus/core/result.hpp"
#include "victus/scene/scene.hpp"
#include "victus/traffic/traffic.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace victus::procgen {

struct CitySettings {
    std::uint32_t seed{1};
    std::uint32_t columns{6}, rows{6}; // blocks along X and Z
    float block_min{60.0f}, block_max{100.0f}; // spacing between road centre lines, m
    std::uint32_t lanes{1};            // street lanes each way
    std::uint32_t avenue_lanes{2};     // avenue lanes each way
    std::uint32_t avenue_every{3};     // every n-th road line is an avenue (0 = none)
    float lane_width{3.5f};
    float sidewalk{3.5f};              // sidewalk width, m
    float kerb_height{0.15f};
    float lot_min{12.0f}, lot_max{26.0f}; // lot frontage, m
    float lot_depth{24.0f};            // deep blocks keep a courtyard behind lots this deep
    float floor_height{3.3f};
    std::uint32_t floors_min{2}, floors_max{28};
    float downtown_radius{0.0f};       // m; 0 = 40% of the city's half-diagonal
    float setback_chance{0.4f};        // tall buildings stepping back in tiers
    float park_chance{0.08f};
    float light_spacing{24.0f};        // street lights along sidewalks (0 = none)
    float tree_spacing{14.0f};         // sidewalk trees (0 = none)
    float speed_limit{13.9f};          // streets, m/s; avenues get 1.25x
};

struct Rect {
    math::Vec2 min, max; // XZ
    [[nodiscard]] math::Vec2 size() const noexcept { return max - min; }
    [[nodiscard]] math::Vec2 center() const noexcept { return (min + max) * 0.5f; }
    [[nodiscard]] float area() const noexcept { return size().x * size().y; }
};

struct Block {
    Rect kerb;      // outer edge of the sidewalk (the kerb line)
    Rect inner;     // buildable area inside the sidewalk
    bool park{false};
};

struct Tier {
    Rect footprint;
    float top{0}; // m above the kerb
};

struct Building {
    std::uint32_t block{0};
    Rect lot;
    std::vector<Tier> tiers; // bottom to top; each inside the one below
    std::uint32_t floors{1};
    std::uint32_t style{0};  // facade palette index (kStyleCount)
    [[nodiscard]] float height() const noexcept { return tiers.empty() ? 0.0f : tiers.back().top; }
};

enum class PropKind : std::uint8_t { StreetLight, Tree };
struct Prop {
    PropKind kind{PropKind::StreetLight};
    math::Vec3 position;
    float yaw{0};   // radians about +Y; street lights face the road along local +Z
    float scale{1};
};

// A closed walking path (sidewalk centre line around one block), counter-clockwise seen from above.
struct WalkLoop {
    std::vector<math::Vec3> points;
    float length{0};
    [[nodiscard]] math::Vec3 point_at(float s, math::Vec3* tangent = nullptr) const;
};

inline constexpr std::uint32_t kStyleCount = 6;

struct City {
    CitySettings settings;
    std::vector<float> xs, zs;          // road centre lines (ascending)
    std::vector<float> x_width, z_width; // full road width of each line (kerb to kerb)
    traffic::RoadNetwork roads;
    std::vector<Block> blocks;
    std::vector<Building> buildings;
    std::vector<Prop> props;
    std::vector<WalkLoop> walks;
    math::Aabb bounds;
    [[nodiscard]] std::uint32_t floors() const noexcept;
};

// Deterministic for a given settings value on every platform (own integer RNG, no std
// distributions). Fails on zero columns/rows, non-positive or inverted ranges, or a block too
// small for its roads and sidewalks.
[[nodiscard]] Result<City> generate_city(const CitySettings& settings);

// ---- meshes ---------------------------------------------------------------------------------

// Glass holds the window panes so they can be drawn glossy (sky reflections) apart from walls.
enum class CityLayer : std::uint8_t { Ground, Buildings, Props, Foliage, Glass };
inline constexpr std::uint32_t kCityLayerCount = 5;

struct CityMesh {
    CityLayer layer{CityLayer::Ground};
    math::Vec2 cell;              // cell index (x, z)
    scene::ProceduralMesh mesh;   // vertex-coloured, unpublished
};

// Merges the city into one mesh per (cell, layer): asphalt with lane markings and crossings,
// kerb plates, parks, building tiers with windows, piers, cornices, shopfronts and roofs, glass
// panes (their own layer), light poles and trees with soft leaf clumps. Cells are
// `cell_size` metres square (<= 0: one cell). Empty meshes are omitted.
[[nodiscard]] std::vector<CityMesh> build_city_meshes(const City& city, float cell_size = 200.0f);

// ---- scene integration ----------------------------------------------------------------------

// Put on an entity to generate a city under it when the simulation starts ("procgen.update"):
// meshes per cell, optional static colliders, a traffic network with `vehicles` agents (the first
// child is the vehicle template, as with TrafficNetwork) and `pedestrians` CrowdAgents walking
// the sidewalks. Generated entities are Transient (not saved). Only the settings are serialized.
struct CityComponent {
    // Mirrors CitySettings field by field (flat for reflection); see there for meaning.
    std::uint32_t seed{1};
    std::uint32_t columns{6}, rows{6};
    float block_min{60.0f}, block_max{100.0f};
    std::uint32_t lanes{1}, avenue_lanes{2}, avenue_every{3};
    float sidewalk{3.5f};
    float lot_min{12.0f}, lot_max{26.0f}, lot_depth{24.0f};
    float floor_height{3.3f};
    std::uint32_t floors_min{2}, floors_max{28};
    float downtown_radius{0.0f};
    float setback_chance{0.4f}, park_chance{0.08f};
    float light_spacing{24.0f}, tree_spacing{14.0f};
    float cell_size{200.0f};
    bool colliders{true};
    std::uint32_t vehicles{60};
    std::uint32_t pedestrians{300};
    Uuid pedestrian_model;        // skinned model drawn by each pedestrian; nil = none spawned
    std::string pedestrian_clip{"Walk"};
    float walk_speed{1.4f};       // m/s average; each pedestrian varies +-20%
    float clip_speed{1.4f};       // m/s the clip was authored at (playback scales with speed)
    bool built{false};            // runtime
    [[nodiscard]] CitySettings settings() const;
};

// Walks a CrowdAgent along a WalkLoop of the scene's city (written by the procgen system).
struct PedestrianComponent {
    std::uint32_t loop{0};
    float distance{0};
    float speed{1.4f};  // negative walks the loop clockwise
    float offset{0};    // m to the right of the loop direction
    bool enabled{true};
};

struct CityStats {
    std::uint32_t blocks{0}, buildings{0}, props{0}, meshes{0}, triangles{0}, colliders{0};
    std::uint32_t vehicles{0}, pedestrians{0};
    double generate_ms{0}, mesh_ms{0};
};

// Scene resource: the generated city of the scene (for scripts, debugging, spawning).
struct CityWorld {
    std::shared_ptr<const City> city;
    scene::Entity owner{};
    CityStats stats;
};

void register_components();

// Generates the city of the first unbuilt CityComponent. Returns false and logs on failure.
bool build_city(scene::Scene& scene);
[[nodiscard]] const City* scene_city(const scene::Scene& scene);
[[nodiscard]] const CityStats* city_stats(const scene::Scene& scene);
// Advances every PedestrianComponent along its loop and writes its Transform.
void update_pedestrians(scene::Scene& scene, float dt);

// Registers "procgen.update" in Phase::Update (before traffic, which it feeds).
void add_procgen_systems(gameplay::Simulation& sim);

} // namespace victus::procgen
