// SPDX-License-Identifier: Apache-2.0
// Traffic (ADR-0167, GAP-22): a road network of intersections and two-way roads, the lane graph
// generated from it (driving lanes along each road, curved connector lanes through each
// intersection, split-phase traffic signals) and lane-following agents that drive it with the
// Intelligent Driver Model (Treiber, Hennecke & Helbing 2000): they keep a safe gap to the vehicle
// ahead, stop at red and amber lights, give way when two lanes merge, and pick a turn at every
// intersection from a deterministic per-agent sequence. Agents are kinematic (they write their
// Transform; no physics bodies), so thousands can drive a city; a player car uses the physics
// vehicle (ADR-0166) instead. Agents follow simulation LOD (ADR-0165): far agents update less
// often with the summed delta, sub-stepped so the model stays stable.
//
// Conventions: Y is up, roads lie on their nodes' heights; traffic drives on the right unless
// `left_hand` is set; an agent's local +Z points along its lane (the vehicle forward axis).
//
// The road network file (`.vroads`) is JSON:
//
//     {"format": "victus.roads", "version": 1,
//      "nodes": [{"position": [x, y, z]}, ...],
//      "roads": [{"a": 0, "b": 1, "lanes": 1, "lanes_back": 1, "lane_width": 3.5, "speed_limit": 13.9}, ...]}
#pragma once
#include "victus/core/json.hpp"
#include "victus/core/math.hpp"
#include "victus/core/result.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/scene/scene.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace victus::traffic {

using scene::Entity;

inline constexpr std::uint32_t kNone = 0xffffffffu;

// ---- road network --------------------------------------------------------------------------

struct RoadNode {
    math::Vec3 position;
};

// A road from node `a` to node `b`. `lanes` drive from a to b, `lanes_back` from b to a (0 makes
// it one-way). Speeds are m/s (13.9 = 50 km/h).
struct Road {
    std::uint32_t a{0}, b{0};
    std::uint32_t lanes{1};
    std::uint32_t lanes_back{1};
    float lane_width{3.5f};
    float speed_limit{13.9f};
};

struct RoadNetwork {
    std::vector<RoadNode> nodes;
    std::vector<Road> roads;
    std::uint32_t add_node(math::Vec3 p) {
        nodes.push_back({p});
        return static_cast<std::uint32_t>(nodes.size() - 1);
    }
    std::uint32_t add_road(const Road& r) {
        roads.push_back(r);
        return static_cast<std::uint32_t>(roads.size() - 1);
    }
};

// A grid of `columns` x `rows` intersections `block` metres apart, centred on `origin`, joined by
// two-way roads with `lanes` lanes each way. The simplest city layout and a test fixture.
[[nodiscard]] RoadNetwork grid_network(std::uint32_t columns, std::uint32_t rows, float block, std::uint32_t lanes = 1,
                                       math::Vec3 origin = {}, float speed_limit = 13.9f);

[[nodiscard]] Result<RoadNetwork> parse_road_network(const json::Value& doc);
[[nodiscard]] Result<RoadNetwork> parse_road_network(std::string_view text);
[[nodiscard]] json::Value road_network_to_json(const RoadNetwork& net);
// Problems that make a network unusable (bad node indices, zero-length roads, no lanes...).
[[nodiscard]] std::vector<std::string> validate(const RoadNetwork& net);

// ---- lane graph ----------------------------------------------------------------------------

struct LaneGraphSettings {
    bool left_hand{false};          // drive on the left (UK, Japan, India...)
    float intersection_radius{0};   // m cleared around nodes with 3+ roads; 0 = from the widest road
    float connector_spacing{1.0f};  // m between points on curved connector lanes
    bool u_turns_at_dead_ends{true};
    // Signal timing for intersections with 3+ roads, seconds per approach.
    float green{12.0f};
    float amber{3.0f};
    float all_red{1.0f};
    bool stagger_signals{true};     // offset each intersection's cycle (deterministic) so they are not in lockstep
};

struct Lane {
    std::vector<math::Vec3> points;  // polyline in driving direction
    std::vector<float> distance;     // cumulative distance at each point
    float length{0};
    float speed_limit{13.9f};
    std::vector<std::uint32_t> next; // lanes reachable from the end
    std::vector<std::uint32_t> prev;
    std::uint32_t road{kNone};       // driving lanes: the road; connectors: kNone
    std::uint32_t node{kNone};       // connectors: the intersection; driving lanes: the node they end at
    std::uint32_t index{0};          // driving lanes: 0 = kerb side lane
    std::uint32_t signal{kNone};     // connectors at signalled intersections: index into LaneGraph::signals
    std::uint32_t phase{0};          // which signal phase lets this connector go
    bool connector{false};
};

// A split-phase controller: each approach road gets green, then amber, then all-red, in turn.
struct Signal {
    std::uint32_t node{0};
    std::uint32_t phases{1};
    float offset{0};                 // seconds into the cycle at time 0
    float green{12}, amber{3}, all_red{1};
    [[nodiscard]] float cycle() const noexcept { return static_cast<float>(phases) * (green + amber + all_red); }
};

enum class Light : std::uint8_t { Green, Amber, Red };

struct LaneGraph {
    std::vector<Lane> lanes;
    std::vector<Signal> signals;
    LaneGraphSettings settings;

    // Position (and unit tangent when `tangent` is set) at distance `s` along `lane`, clamped.
    [[nodiscard]] math::Vec3 point_at(std::uint32_t lane, float s, math::Vec3* tangent = nullptr) const;
    // State of the light for connector `lane` at simulation time `t`; connectors without a
    // signal and driving lanes are always Green.
    [[nodiscard]] Light light(std::uint32_t lane, double t) const;
    [[nodiscard]] float total_length() const;
    // Lanes ending within `radius` of `p`, nearest first (for spawning and debugging).
    [[nodiscard]] std::uint32_t nearest_lane(math::Vec3 p, float* s_out = nullptr) const;
};

[[nodiscard]] LaneGraph build_lane_graph(const RoadNetwork& net, const LaneGraphSettings& settings = {});

// Line segments for overlays: every lane (connectors included) as a polyline, as pairs of points.
[[nodiscard]] std::vector<math::Vec3> lane_graph_lines(const LaneGraph& graph);

// ---- agents --------------------------------------------------------------------------------

// A lane-following vehicle. Authored fields shape its driving; the runtime fields (lane,
// distance, speed, next) are written by the traffic system and serialized so saves resume.
struct TrafficAgentComponent {
    float max_speed{15.0f};       // m/s desired speed on an open road (also capped by the lane limit)
    float acceleration{1.5f};     // m/s^2 comfortable acceleration (IDM a)
    float deceleration{3.0f};     // m/s^2 comfortable braking (IDM b)
    float time_headway{1.4f};     // s gap kept to the vehicle ahead (IDM T)
    float min_gap{2.0f};          // m bumper-to-bumper gap when stopped (IDM s0)
    float length{4.5f};           // m
    std::uint32_t seed{0};        // turn choices; 0 = from the entity index
    bool enabled{true};
    // Runtime
    std::uint32_t lane{kNone};
    float distance{0};            // m along the lane (front bumper)
    float speed{0};               // m/s
    std::uint32_t next{kNone};    // lane chosen after this one
    std::uint32_t turns{0};       // lanes driven (advances the turn sequence)
    friend bool operator==(const TrafficAgentComponent&, const TrafficAgentComponent&) = default;
};

struct TrafficStats {
    std::uint32_t agents{0};
    std::uint32_t updated{0};          // agents simulated this frame (simulation LOD)
    std::uint32_t stopped{0};          // speed < 0.5 m/s
    std::uint32_t waiting_at_light{0}; // stopped by a red or amber light
    std::uint32_t lane_changes{0};     // lane transitions this frame
    float mean_speed{0};               // m/s over agents
    double update_ms{0};
};

// Scene resource holding the active graph and the simulation clock for signals.
struct TrafficWorld {
    std::shared_ptr<const LaneGraph> graph;
    double time{0};
    TrafficStats stats;
};

void register_components();

// Makes `graph` the scene's traffic network (replacing any previous one) and resets the clock.
void set_traffic_graph(scene::Scene& scene, std::shared_ptr<const LaneGraph> graph);
[[nodiscard]] const LaneGraph* traffic_graph(const scene::Scene& scene);
[[nodiscard]] const TrafficStats* traffic_stats(const scene::Scene& scene);

// Places `count` agents on the graph's driving lanes, spaced evenly by length (deterministic for
// a seed), as children of `parent` (or roots). Each gets a Transform and a TrafficAgentComponent;
// add a MeshRenderer or prefab yourself. Returns the entities created.
std::vector<Entity> spawn_traffic(scene::Scene& scene, std::uint32_t count, std::uint32_t seed = 1, Entity parent = {});

// Advances every agent by `dt` and writes their Transforms (what the "traffic.update" system runs).
void update_traffic(scene::Scene& scene, float dt);

// ---- authored networks -----------------------------------------------------------------------

// Put on an entity to give a scene traffic without code. On the first update the network is
// loaded from `network` (a .vroads asset, relative to the assets folder) or, when that is empty,
// generated as a `columns` x `rows` grid; the lane graph is built and `vehicles` agents are
// spawned as children of this entity. When the entity has a child, the first child is the vehicle
// template: each agent gets a copy of it (meshes, scripts...) as a child, and the template is
// hidden (its meshes are made invisible). The template's local transform is kept as each copy's
// offset from its car, so author it at the origin. The graph is in this entity's local space.
struct TrafficNetworkComponent {
    std::string network;          // .vroads asset; empty = grid
    std::uint32_t columns{4}, rows{4};
    float block{80.0f};           // m between grid intersections
    std::uint32_t lanes{1};       // grid lanes each way
    float speed_limit{13.9f};     // grid roads, m/s
    std::uint32_t vehicles{40};
    std::uint32_t seed{1};
    bool left_hand{false};
    float green{12.0f};           // seconds of green per approach
    bool built{false};            // runtime: set once the network is active
};

using NetworkLoader = std::function<Result<std::string>(std::string_view path)>;
// Reads .vroads files relative to an assets folder (paths are sanitized).
[[nodiscard]] NetworkLoader directory_loader(std::filesystem::path assets_root);

// Builds the scene's network from its first TrafficNetworkComponent if one is waiting (what the
// "traffic.update" system does before advancing agents). Returns false and logs on a load error.
bool build_authored_network(scene::Scene& scene, const NetworkLoader& loader = {});

// Registers "traffic.update" in Phase::Update.
void add_traffic_systems(gameplay::Simulation& sim, NetworkLoader loader = {});

} // namespace victus::traffic
