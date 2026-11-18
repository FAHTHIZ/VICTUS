// SPDX-License-Identifier: Apache-2.0
// CPU particle emitters for sparks, fire, smoke, dust and pickups.
//
// The module is headless and deterministic: every emitter owns a PCG32 stream seeded from its
// `seed` (or its entity UUID), and emitters are simulated independently, so a run produces
// bit-identical particles whether emitters are updated serially or on the job system.
// Each update also produces camera-independent billboards (world position, size, colour,
// rotation) that the runtime hands to the renderer (3D: depth-aware soft particles; 2D: sprites).
// Textures can be flipbooks (a grid of frames played over each particle's life or at a fixed
// rate), and particles can stretch along their velocity (sparks, rain, streaks).
#pragma once
#include "victus/core/hash.hpp"
#include "victus/core/math.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/scene/scene.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace victus::jobs {
class JobSystem;
}

namespace victus::particles {

enum class Shape : std::uint8_t { Point, Sphere, Cone, Box };
enum class Blend : std::uint8_t { Alpha, Additive };

// Emits along the entity's local +Y axis (Sphere emits outward in all directions).
struct ParticleEmitterComponent {
    bool emitting{true};
    bool loop{true};           // with duration > 0: restart the cycle (and its burst) when it ends
    bool world_space{true};    // false: particles follow the entity
    float duration{0};         // cycle length in seconds; 0 = emit forever
    float rate{20};            // particles per second while emitting
    std::uint32_t burst{0};    // particles emitted at the start of every cycle
    std::uint32_t max_particles{256};
    float lifetime_min{0.8f}, lifetime_max{1.2f};
    Shape shape{Shape::Cone};
    float radius{0.1f};        // sphere radius / cone base radius
    float angle{20};           // cone spread half-angle in degrees (Point, Cone, Box)
    math::Vec3 box{1, 1, 1};   // Box: full size
    float speed_min{1}, speed_max{2};
    math::Vec3 gravity{0, 0, 0}; // acceleration in m/s^2
    float drag{0};             // velocity damping per second
    float size_start{0.2f}, size_end{0.05f};
    float size_variation{0};   // 0..1 random per-particle size scale reduction
    math::Color color_start{1, 1, 1, 1};
    math::Color color_end{1, 1, 1, 0};
    float intensity{1};        // HDR multiplier on colour; > 1 glows through bloom
    float spin_min{0}, spin_max{0}; // degrees per second
    Blend blend{Blend::Alpha};
    Uuid texture;              // optional texture asset; nil = built-in soft disc
    float softness{0.25f};     // metres over which particles fade into geometry
    std::int32_t layer{0};     // 2D sprite layer
    std::uint32_t seed{0};     // 0 = derived from the entity UUID
    // Flipbook: the texture is a grid of frames, left to right then top to bottom.
    std::uint32_t flipbook_columns{1}, flipbook_rows{1}; // 1 x 1 = whole texture; at most 15 x 15
    std::uint32_t flipbook_frames{0}; // frames used (0 = columns x rows)
    float flipbook_fps{0};            // 0: frames span each particle's life once; > 0: loop at this rate
    bool flipbook_random_start{false}; // each particle starts on a random frame
    // Velocity stretch: a moving particle's length becomes size + speed x velocity_stretch
    // (seconds), aligned with its on-screen motion. 0 = square billboards that keep their spin.
    float velocity_stretch{0};
};

// Frame shown at `age` of `life` seconds, given the particle's start frame (exposed for tests).
[[nodiscard]] std::uint32_t flipbook_frame(const ParticleEmitterComponent& e, float age, float life, std::uint32_t start);
// Usable frame count (columns x rows, clamped by flipbook_frames); 1 when the flipbook is off.
[[nodiscard]] std::uint32_t flipbook_frame_count(const ParticleEmitterComponent& e);

// One particle ready for drawing. Layout matches render::ParticleInstance (48 bytes).
struct Billboard {
    math::Vec3 position;
    float size{0};
    math::Color color;
    float rotation{0}; // radians (screen plane; stretched particles point along their velocity)
    float frame{0};    // flipbook frame
    float grid{0};     // flipbook grid = columns * 16 + rows (0 = whole texture)
    float aspect{0};   // height / width along the rotated up axis (0 = square)
};
static_assert(sizeof(Billboard) == 48);

struct EmitterView {
    scene::Entity entity;
    const ParticleEmitterComponent* emitter{nullptr};
    std::span<const Billboard> billboards;
    std::span<const math::Vec3> velocities; // world space, parallel to billboards; empty unless stretched
    math::Aabb bounds; // world space; invalid when empty
};

struct EmitterStatus {
    bool valid{false};
    std::uint32_t alive{0};
    std::uint64_t emitted{0}; // total since start
    float time{0};            // seconds into the current cycle
    bool finished{false};     // non-looping cycle ended and every particle died
};

struct ParticleStats {
    std::uint32_t emitters{0}, alive{0}, spawned{0}, died{0};
};

struct ParticleSettings {
    jobs::JobSystem* jobs{nullptr};    // simulates emitters in parallel when set
    std::uint32_t parallel_threshold{4}; // minimum emitters before using jobs
};

class ParticleSystem {
public:
    explicit ParticleSystem(ParticleSettings settings = {});
    ~ParticleSystem();
    ParticleSystem(const ParticleSystem&) = delete;
    ParticleSystem& operator=(const ParticleSystem&) = delete;
    void update(scene::Scene& scene, float dt);
    [[nodiscard]] const ParticleStats& stats() const noexcept { return stats_; }

private:
    ParticleSettings settings_;
    ParticleStats stats_;
    struct Work;
    std::vector<Work> work_;
};

void register_components();
// Installs "particles.update" (Phase::PostUpdate, after scripts and physics moved the emitters).
void add_particle_systems(gameplay::Simulation& sim, ParticleSystem& system);

// Queues `count` extra particles on the next update (emitter need not be emitting).
bool emit(scene::Scene& scene, scene::Entity e, std::uint32_t count);
// Kills every live particle and restarts the cycle.
bool reset(scene::Scene& scene, scene::Entity e);
[[nodiscard]] EmitterStatus status(scene::Scene& scene, scene::Entity e);
// Re-aims a stretched billboard (aspect > 1) along its velocity's on-screen direction for a camera
// with view matrix `view`, shortening the streak as the motion turns toward the view axis.
void aim_stretched(Billboard& b, const math::Vec3& velocity, const math::Mat4& view);
// Texture rectangle (u0, v0 top, u1, v1 bottom) of a billboard's flipbook frame; whole texture when off.
[[nodiscard]] math::Vec4 flipbook_uv(const Billboard& b);
// Every emitter with its billboards from the last update.
void gather(scene::Scene& scene, std::vector<EmitterView>& out);

} // namespace victus::particles
