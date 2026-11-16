// SPDX-License-Identifier: Apache-2.0
// Navigation: navmesh generation from a scene's static colliders (Recast), path queries
// (Detour) and agents that steer along paths with local avoidance (DetourCrowd).
//
// A scene opts in by having NavAgent entities or by calling the query API; the navmesh is built
// on demand from every static, non-trigger 3D Collider (no RigidBody, or a Static one) and
// rebuilt when those colliders change. An optional NavMeshSurface component (any entity, the
// first one found) sets the agent dimensions the mesh is built for. Agents with a
// CharacterController are driven through PhysicsWorld::move_character (the controller handles
// gravity, steps and collisions); other agents are placed on the navmesh surface directly.
#pragma once
#include "victus/core/result.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/scene/scene.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace victus::navigation {

using scene::Entity;

// Build settings for the scene's navmesh (Recast parameters in metres/degrees).
struct NavMeshSurfaceComponent {
    float agent_radius{0.4f};   // clearance kept from walls and obstacles
    float agent_height{1.8f};   // minimum free height
    float max_climb{0.4f};      // highest ledge/step walked up or down
    float max_slope{45.0f};     // degrees
    float cell_size{0.15f};     // horizontal voxel size (smaller = more accurate, slower build)
    float cell_height{0.1f};    // vertical voxel size
    std::uint32_t layer_mask{0xFFFFu}; // collider layers (bit per layer) that contribute geometry
    bool auto_rebuild{true};    // rebuild when static colliders change
    friend bool operator==(const NavMeshSurfaceComponent&, const NavMeshSurfaceComponent&) = default;
};

// An entity that walks the navmesh. Give it a destination from Luau (`entity:set_destination`)
// or C++ (NavigationSystem::set_destination).
struct NavAgentComponent {
    float speed{3.5f};             // m/s
    float acceleration{12.0f};     // m/s^2
    float radius{0.4f};            // avoidance radius
    float height{1.8f};
    float stopping_distance{0.2f}; // arrival tolerance (m)
    float base_offset{0.0f};       // entity origin above the navmesh (agents without a CharacterController)
    bool avoidance{true};          // steer around other agents
    bool rotate{true};             // turn to face the movement direction (yaw)
    float turn_speed{540.0f};      // degrees per second
    friend bool operator==(const NavAgentComponent&, const NavAgentComponent&) = default;
};

struct NavStats {
    bool valid{false};               // a navmesh exists
    std::uint32_t source_triangles{0};
    std::uint32_t polygons{0};
    std::uint32_t vertices{0};
    std::uint32_t builds{0};         // total builds since creation
    double build_ms{0};
    std::uint32_t agents{0};
    std::uint32_t moving{0};
};

struct AgentState {
    bool active{false};           // registered on the navmesh
    bool has_destination{false};
    bool reached{false};          // arrived within stopping_distance (cleared by a new destination)
    bool partial{false};          // the destination is unreachable; heading to the closest point
    math::Vec3 destination;
    math::Vec3 velocity;
    float remaining_distance{0};  // straight-line distance to the destination
};

struct RaycastResult {
    bool hit{false};        // the ray left the navmesh (hit a wall/edge) before reaching `to`
    math::Vec3 point;       // where it stopped
    math::Vec3 normal;      // wall normal at the hit (xz)
};

struct DebugLine {
    math::Vec3 a, b;
    math::Color color;
};

struct NavigationSettings {
    std::uint32_t max_agents{256};
    float rebuild_check_interval{0.25f}; // seconds between change checks when auto_rebuild is on
};

class NavigationSystem {
public:
    explicit NavigationSystem(NavigationSettings settings = {});
    ~NavigationSystem();
    NavigationSystem(const NavigationSystem&) = delete;
    NavigationSystem& operator=(const NavigationSystem&) = delete;

    // Builds (or rebuilds) the navmesh from the scene's static colliders. Agents are re-added.
    [[nodiscard]] Status build(scene::Scene& scene);
    // Builds when there is no navmesh yet or static geometry changed since the last build.
    [[nodiscard]] Status ensure_built(scene::Scene& scene);
    [[nodiscard]] bool valid() const noexcept;

    // Straight path (corner points, start and end included). False when either end is off the
    // navmesh; `partial` is set when the end is unreachable and the path leads to the closest point.
    bool find_path(math::Vec3 from, math::Vec3 to, std::vector<math::Vec3>& out, bool* partial = nullptr) const;
    // Closest navmesh point within `extent` (half size of the search box).
    [[nodiscard]] std::optional<math::Vec3> nearest(math::Vec3 point, math::Vec3 extent = {2, 4, 2}) const;
    [[nodiscard]] RaycastResult raycast(math::Vec3 from, math::Vec3 to) const;
    // Uniformly random navmesh point (deterministic for a given seed).
    [[nodiscard]] std::optional<math::Vec3> random_point(std::uint32_t seed) const;

    // Agents (entities with NavAgentComponent).
    bool set_destination(scene::Scene& scene, Entity agent, math::Vec3 destination);
    void stop(scene::Scene& scene, Entity agent);
    [[nodiscard]] AgentState agent_state(Entity agent) const;

    // Syncs agents with the scene, steers them and moves their entities.
    void update(scene::Scene& scene, float dt);

    // Navmesh polygon outlines (and agent paths) for debug overlays.
    void debug_lines(std::vector<DebugLine>& out, bool include_paths = true) const;
    [[nodiscard]] const NavStats& stats() const noexcept { return stats_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    NavStats stats_;
};

// Registers "NavMeshSurface" and "NavAgent" (inspector category "Navigation"). Idempotent.
void register_components();
// Installs "navigation.update" (Phase::Update, after scripts so destinations set this frame apply).
void add_navigation_systems(gameplay::Simulation& sim, NavigationSystem& nav);
// The navigation system driving `scene` (set by the installed systems), for scripts.
[[nodiscard]] NavigationSystem* navigation_of(scene::Scene& scene);
void bind(scene::Scene& scene, NavigationSystem* nav);

} // namespace victus::navigation
