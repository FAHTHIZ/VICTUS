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
#include "victus/scene/reflect.hpp"
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
enum class SubEmitterTrigger : std::uint8_t { Death, Birth };
// What an emitter does while no viewer can see its bounds (ADR-0210).
enum class Offscreen : std::uint8_t { Simulate, Throttle, Pause };
// Simulation level an emitter ran at in the last update (ADR-0210).
enum class Lod : std::uint8_t { Full, Throttled, Paused, Culled };

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
    // Over-life curves (ADR-0209). Time is the particle's age / lifetime (0..1); keys ascending,
    // at most kMaxCurveKeys used. A non-empty colour gradient replaces color_start -> color_end
    // (fire: white -> yellow -> orange -> dark red -> transparent); a non-empty size curve makes
    // the size size_start x curve(t) instead of size_start -> size_end (smoke puffs that grow fast
    // then slowly, sparks that pop then shrink). Empty = the linear start/end behaviour.
    std::vector<scene::GradientKey> color_over_life;
    std::vector<math::Vec2> size_over_life;
    // Motion multipliers over life (ADR-0211), same key rules; empty = 1. speed_over_life scales
    // how far the particle moves along its velocity each step (forces still act on the velocity),
    // so sparks can burst out then crawl and embers can slow before rising again; spin_over_life
    // scales the spin (debris that tumbles fast then settles).
    std::vector<math::Vec2> speed_over_life;
    std::vector<math::Vec2> spin_over_life;
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
    // Trails: each particle leaves a camera-facing ribbon along its path. A new trail point is
    // recorded once the particle has moved `trail_min_distance` from the last one; points older
    // than `trail_lifetime` seconds expire, and the trail dies with its particle.
    float trail_lifetime{0};          // seconds; 0 = no trails
    float trail_min_distance{0.05f};  // metres between recorded points
    std::uint32_t trail_max_points{16}; // recorded points per particle (2..64; oldest dropped when full)
    float trail_width{1};             // trail width at the head, x particle size
    float trail_width_end{0};         // width at the tail as a fraction of the head width
    float trail_alpha_end{0};         // alpha at the tail as a fraction of the particle's alpha
    bool trail_billboards{true};      // also draw the particles themselves
    // Turbulence (both paths): a divergence-free curl-noise acceleration field (Bridson et al. 2007)
    // that swirls particles without bunching them. Units: m/s^2 at peak; frequency in cycles per
    // metre; speed = how fast the field evolves (radians per second of field phase).
    float turbulence{0};
    float turbulence_frequency{0.5f};
    float turbulence_speed{1};
    // GPU simulation (ADR-0097, 3D views only): the CPU keeps the emission schedule and the exact
    // slot/lifetime bookkeeping, the renderer integrates and draws every particle in compute
    // shaders. Up to kMaxGpuParticles per emitter; per-particle CPU readback is not available.
    // Alpha-blended GPU emitters are depth sorted on the GPU every frame (ADR-0107). Trails are
    // recorded and built on the GPU (ADR-0110), up to 2^18 segments per emitter.
    bool gpu{false};
    // Depth-buffer collision (GPU emitters in world space): particles bounce off what the camera
    // sees, using the scene depth of the frame. Off-screen or hidden particles do not collide.
    bool collision{false};
    float bounce{0.4f};               // 0 = stop, 1 = perfectly elastic (normal component)
    float friction{0.2f};             // fraction of tangential speed removed per contact
    float collision_thickness{0.5f};  // metres behind the visible surface still treated as solid
    // Sub-emitter (ADR-0108, CPU source emitters): when one of this emitter's particles dies (or
    // is born), the `sub_emitter` entity's emitter spawns `sub_emitter_count` particles from its
    // own shape, placed at that particle's position and oriented like the target entity, adding
    // `sub_emitter_inherit_velocity` x the particle's velocity. Delivered on the target's next
    // update (one step later), in a fixed order, so runs stay deterministic. The target may be a
    // CPU or GPU emitter, and may have its own sub-emitter; an emitter cannot target itself.
    Uuid sub_emitter;
    SubEmitterTrigger sub_emitter_trigger{SubEmitterTrigger::Death};
    std::uint32_t sub_emitter_count{8};
    float sub_emitter_inherit_velocity{0};
    // Simulation LOD (ADR-0210), measured from the nearest viewer (ParticleSystem::set_viewers, or
    // every primary camera in the scene). No viewers: every emitter runs at full rate.
    //   cull_distance: farther than this, the emitter's particles are removed and it stops
    //     emitting until a viewer comes back within range (its cycle and bursts restart).
    //   lod_distance: farther than this, the emitter updates only lod_rate times per second (with
    //     the elapsed time, so motion and lifetimes stay correct) and spawns lod_spawn_scale of its
    //     rate and bursts.
    //   offscreen: no viewer's frustum touches its bounds: keep simulating, throttle like a
    //     distant emitter, or pause (frozen, no spawning) until seen again.
    float cull_distance{0};   // metres; 0 = never
    float lod_distance{0};    // metres; 0 = never throttle by distance
    float lod_rate{15};       // updates per second while throttled
    float lod_spawn_scale{0.5f}; // 0..1 fraction of rate and bursts while throttled
    Offscreen offscreen{Offscreen::Simulate};
};

// A point of view for particle LOD: its position, and optionally the frustum it renders.
struct ParticleViewer {
    math::Vec3 position;
    math::Frustum frustum{};
    bool has_frustum{false}; // false: sees every emitter (distance rules still apply)
};

constexpr std::uint32_t kMaxCpuParticles = 65536;
constexpr std::uint32_t kMaxGpuParticles = 1u << 20;
// Keys of color_over_life / size_over_life that are evaluated (both paths).
constexpr std::size_t kMaxCurveKeys = 8;
// Sub-emitter events one emitter can queue per update (further events are dropped).
constexpr std::uint32_t kMaxSubEmitterEvents = 4096;

// Curl-noise acceleration of `e` at `position` (simulation space) and emitter time `time`
// (seconds). Exactly divergence-free. Mirrored by engine/render/shaders/particle_sim.comp.
[[nodiscard]] math::Vec3 turbulence_at(const ParticleEmitterComponent& e, const math::Vec3& position, float time);
// Phase used by the field for emitter time `time` (wrapped so float precision holds for days).
[[nodiscard]] float turbulence_phase(const ParticleEmitterComponent& e, double time);

// Colour (before intensity, alpha unclamped) and size factor (before the per-particle scale) at
// normalised age t. Mirrored by particle_sim.comp.
[[nodiscard]] math::Color color_at(const ParticleEmitterComponent& e, float t);
[[nodiscard]] float size_at(const ParticleEmitterComponent& e, float t);
// Largest size_at over the particle's life (bounds).
[[nodiscard]] float max_size(const ParticleEmitterComponent& e);
// Speed and spin multipliers at normalised age t (1 when the curve is empty; ADR-0211).
[[nodiscard]] float speed_at(const ParticleEmitterComponent& e, float t);
[[nodiscard]] float spin_at(const ParticleEmitterComponent& e, float t);
// Largest |speed_at| over the particle's life (bounds).
[[nodiscard]] float max_speed_scale(const ParticleEmitterComponent& e);
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

// One trail point, tail to head within its strip. Layout matches render::RibbonPoint (48 bytes).
struct RibbonPoint {
    math::Vec3 position; // world space
    float width{0};      // full width in metres
    math::Color color;
    float v{0};          // 0 at the head, 1 at the end of the trail lifetime
    float reserved[3]{};
};
static_assert(sizeof(RibbonPoint) == 48);

// GPU particle state, one per slot (64 bytes, std430). Layout matches the `Particle` struct in
// particle_sim.comp and render::GpuParticleSpawn (slot in meta.x for spawns).
struct GpuParticle {
    math::Vec3 position; // simulation space (world, or emitter-local without world_space)
    float age{0};        // seconds; dead when age >= life
    math::Vec3 velocity;
    float life{0};
    float rotation{0}, spin{0}, size_scale{1}, start_frame{0};
    std::uint32_t meta[4]{}; // x: slot (spawn records) / birth serial (GPU state)
};
static_assert(sizeof(GpuParticle) == 64);

// A particle the CPU spawned for the GPU: initial state at `time` (emitter clock), in slot
// `particle.meta[0]`. Layout matches render::GpuParticleSpawnRecord (80 bytes).
struct GpuSpawnRecord {
    GpuParticle particle;
    std::uint64_t index{0}; // spawn sequence number (0, 1, 2, ...)
    double time{0};         // emitter clock at birth
};
static_assert(sizeof(GpuSpawnRecord) == 80);

// What a renderer needs to keep its GPU copy of an emitter in step with the CPU schedule.
struct GpuEmitterView {
    std::uint64_t key{0};        // stable per entity (from its UUID)
    std::uint64_t instance{0};   // changes when the emitter state is recreated (scene reload, play)
    std::uint32_t generation{0}; // bumped by reset() or a capacity change: drop every particle
    std::uint32_t capacity{0};   // slots
    std::uint32_t alive{0};      // exact live count (CPU bookkeeping)
    double time{0};              // emitter clock after the last update
    std::uint64_t spawned{0};    // spawn records issued so far (next index)
    std::span<const GpuSpawnRecord> records; // recent spawns, ascending index (oldest may be trimmed)
    math::Mat4 world{math::Mat4::identity()}; // emitter transform (local-space emitters)
};

struct EmitterView {
    scene::Entity entity;
    const ParticleEmitterComponent* emitter{nullptr};
    std::span<const Billboard> billboards;
    std::span<const math::Vec3> velocities; // world space, parallel to billboards; empty unless stretched
    math::Aabb bounds; // world space (billboards and trails); invalid when empty
    std::span<const RibbonPoint> ribbon_points; // every trail, tail to head
    std::span<const std::uint32_t> ribbon_strips; // point count of each trail (>= 2)
    const GpuEmitterView* gpu{nullptr}; // GPU emitters: simulated by the renderer (billboards empty)
};

struct EmitterStatus {
    bool valid{false};
    std::uint32_t alive{0};
    std::uint64_t emitted{0}; // total since start
    float time{0};            // seconds into the current cycle
    bool finished{false};     // non-looping cycle ended and every particle died
    Lod lod{Lod::Full};       // simulation level in the last update (ADR-0210)
    float viewer_distance{-1}; // metres to the nearest viewer in the last update; -1 = no viewers
};

struct ParticleStats {
    std::uint32_t emitters{0}, alive{0}, spawned{0}, died{0};
    std::uint32_t gpu_emitters{0}, gpu_alive{0}; // subset simulated on the GPU
    std::uint32_t sub_emitter_events{0};  // source particle births/deaths delivered to a target this update
    std::uint32_t sub_emitter_dropped{0}; // events with no valid target (missing, self, not an emitter) or over the target's queue
    // Simulation LOD (ADR-0210): emitters per level this update, and throttled emitters that
    // skipped this update (waiting for their next lod_rate step).
    std::uint32_t lod_full{0}, lod_throttled{0}, lod_paused{0}, lod_culled{0}, lod_skipped{0};
    std::uint32_t viewers{0};
};

struct ParticleSettings {
    jobs::JobSystem* jobs{nullptr};    // simulates emitters in parallel when set
    std::uint32_t parallel_threshold{4}; // minimum emitters before using jobs
    // Width / height assumed for automatic camera viewers (the simulation does not know the
    // window): wide by default so off-screen tests stay conservative on any screen.
    float viewer_aspect{2.4f};
};

class ParticleSystem {
public:
    explicit ParticleSystem(ParticleSettings settings = {});
    ~ParticleSystem();
    ParticleSystem(const ParticleSystem&) = delete;
    ParticleSystem& operator=(const ParticleSystem&) = delete;
    void update(scene::Scene& scene, float dt);
    [[nodiscard]] const ParticleStats& stats() const noexcept { return stats_; }
    // Viewers for simulation LOD (ADR-0210); replaces the automatic scene-camera viewers until
    // use_scene_cameras(). An empty span means "no viewers": every emitter at full rate.
    void set_viewers(std::span<const ParticleViewer> viewers);
    void use_scene_cameras();
    [[nodiscard]] std::span<const ParticleViewer> viewers() const noexcept { return viewers_; }

private:
    ParticleSettings settings_;
    ParticleStats stats_;
    std::vector<ParticleViewer> viewers_;
    bool explicit_viewers_{false};
    struct Work;
    std::vector<Work> work_;
};

void register_components();
// Primary cameras of `scene` as viewers (perspective or orthographic frustum at `aspect`).
[[nodiscard]] std::vector<ParticleViewer> camera_viewers(scene::Scene& scene, float aspect);
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
// Reference integration of one GPU particle over `dt` seconds starting at emitter time `time`
// (exactly what particle_sim.comp does, minus depth collision); used by tests and tooling.
void gpu_integrate(const ParticleEmitterComponent& e, GpuParticle& p, float dt, double time);
// Billboard the GPU writes for a particle (world = emitter transform, view = camera view for
// stretched particles). Dead particles give size 0.
[[nodiscard]] Billboard gpu_billboard(const ParticleEmitterComponent& e, const GpuParticle& p, const math::Mat4& world,
                                      const math::Mat4& view);
// Every emitter with its billboards from the last update. Billboards are empty when
// trail_billboards is off; trails only appear in ribbon_points.
void gather(scene::Scene& scene, std::vector<EmitterView>& out);

} // namespace victus::particles
