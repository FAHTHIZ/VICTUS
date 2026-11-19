// SPDX-License-Identifier: Apache-2.0
#include "victus/particles/particles.hpp"

#include "victus/core/jobs.hpp"
#include "victus/scene/reflect.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <functional>
#include <mutex>

namespace victus::particles {

using math::Aabb;
using math::Color;
using math::Mat4;
using math::Vec3;
using scene::Entity;
using scene::Scene;

namespace {

constexpr std::uint32_t kHardCap = kMaxCpuParticles;
constexpr float kPi = 3.14159265358979f;
// Spawn records kept for renderers that join late or skip frames (live particles only).
constexpr std::size_t kMaxRetainedRecords = 65536;
std::atomic<std::uint64_t> g_instances{1};

struct Particle {
    Vec3 position;
    float age{0};
    Vec3 velocity;
    float life{1};
    float rotation{0};
    float spin{0};
    float size_scale{1};
    std::uint32_t start_frame{0};
    std::uint32_t trail_count{0}; // recorded trail points (in EmitterState::trail)
};

struct TrailPoint {
    Vec3 position; // simulation space (world or emitter-local)
    float age{0};  // particle age when recorded
};

// CPU bookkeeping of a GPU emitter: which slots are live until when, and recent spawns.
struct GpuBook {
    std::uint32_t capacity{0};
    std::uint32_t generation{0};
    std::uint64_t instance{0};
    double time{0};
    std::uint64_t spawned{0};
    std::vector<std::pair<double, std::uint32_t>> live; // min-heap on death time
    std::vector<std::uint32_t> free;                    // stack, lowest slot on top initially
    std::vector<GpuSpawnRecord> records;                // ascending index from `head`
    std::size_t head{0};
    std::deque<std::pair<double, Aabb>> boxes; // spawn-position boxes until their last particle dies
    Mat4 prev_world{Mat4::identity()};
    bool has_prev{false};
    GpuEmitterView view;
};

// A source particle's birth or death, in world space, waiting for the target emitter.
struct SubEvent {
    Vec3 position;
    Vec3 velocity; // already scaled by the source's sub_emitter_inherit_velocity
    std::uint32_t count{0};
};

// Runtime state, not serialized: rebuilt (empty) after loading.
struct EmitterState {
    std::vector<Particle> particles;
    std::vector<Billboard> billboards;
    std::vector<Vec3> velocities;
    std::vector<TrailPoint> trail; // trail_cap points per particle, parallel to particles
    std::uint32_t trail_cap{0};
    std::vector<RibbonPoint> ribbon_points;
    std::vector<std::uint32_t> ribbon_strips;
    Aabb bounds;
    Random rng{1, 1};
    bool seeded{false};
    bool started{false};
    bool finished{false};
    float time{0};
    float accumulator{0};
    std::uint32_t pending{0};
    std::uint64_t emitted{0};
    double clock{0}; // seconds since the emitter started (turbulence phase)
    bool gpu{false}; // simulated by the last update on the GPU path
    GpuBook book;
    std::vector<SubEvent> events;   // produced by the last update (this emitter as a source)
    std::vector<SubEvent> incoming; // delivered by sources, spawned on the next update
};

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

Vec3 cone_direction(Random& rng, float half_angle_degrees) {
    const float cos_max = std::cos(std::clamp(half_angle_degrees, 0.0f, 180.0f) * (kPi / 180.0f));
    const float c = 1.0f - rng.next_float() * (1.0f - cos_max);
    const float s = std::sqrt(std::max(0.0f, 1.0f - c * c));
    const float phi = rng.next_float() * 2.0f * kPi;
    return {s * std::cos(phi), c, s * std::sin(phi)};
}

Vec3 sphere_direction(Random& rng) {
    const float z = rng.next_float() * 2.0f - 1.0f;
    const float s = std::sqrt(std::max(0.0f, 1.0f - z * z));
    const float phi = rng.next_float() * 2.0f * kPi;
    return {s * std::cos(phi), s * std::sin(phi), z};
}

// Spawn position and direction in emitter-local space.
void sample_shape(const ParticleEmitterComponent& e, Random& rng, Vec3& pos, Vec3& dir) {
    switch (e.shape) {
    case Shape::Point:
        pos = {};
        dir = cone_direction(rng, e.angle);
        break;
    case Shape::Sphere: {
        dir = sphere_direction(rng);
        pos = dir * (std::max(0.0f, e.radius) * std::cbrt(rng.next_float()));
        break;
    }
    case Shape::Cone: {
        const float r = std::max(0.0f, e.radius) * std::sqrt(rng.next_float());
        const float phi = rng.next_float() * 2.0f * kPi;
        pos = {r * std::cos(phi), 0.0f, r * std::sin(phi)};
        dir = cone_direction(rng, e.angle);
        break;
    }
    case Shape::Box:
        pos = {(rng.next_float() - 0.5f) * e.box.x, (rng.next_float() - 0.5f) * e.box.y, (rng.next_float() - 0.5f) * e.box.z};
        dir = cone_direction(rng, e.angle);
        break;
    }
}

std::uint32_t grid_axis(std::uint32_t n) { return std::clamp<std::uint32_t>(n, 1, 15); }

bool has_sub_emitter(const ParticleEmitterComponent& e) { return !e.sub_emitter.is_nil() && e.sub_emitter_count > 0; }

void push_event(const ParticleEmitterComponent& e, EmitterState& s, const Vec3& position, const Vec3& velocity) {
    if (s.events.size() >= kMaxSubEmitterEvents) return;
    const float inherit = std::isfinite(e.sub_emitter_inherit_velocity) ? e.sub_emitter_inherit_velocity : 0.0f;
    s.events.push_back({position, velocity * inherit, std::min(e.sub_emitter_count, kMaxCpuParticles)});
}

// Placement of a sub-emitter spawn: the target's shape sample `pos`/`dir` (emitter-local) moved to
// the event position, oriented by the target transform. Results are in simulation space.
void place_sub(const ParticleEmitterComponent& e, const Mat4& world, const SubEvent& ev, const Vec3& pos, const Vec3& dir,
               float speed, Vec3& out_pos, Vec3& out_vel) {
    const Vec3 wp = ev.position + world.transform_vector(pos);
    const Vec3 wv = math::normalize(world.transform_vector(dir)) * speed + ev.velocity;
    if (e.world_space) {
        out_pos = wp;
        out_vel = wv;
    } else {
        const Mat4 inv = math::inverse(world);
        out_pos = inv.transform_point(wp);
        out_vel = inv.transform_vector(wv);
    }
}

} // namespace

std::uint32_t flipbook_frame_count(const ParticleEmitterComponent& e) {
    const std::uint32_t all = grid_axis(e.flipbook_columns) * grid_axis(e.flipbook_rows);
    return e.flipbook_frames > 0 ? std::min(e.flipbook_frames, all) : all;
}

std::uint32_t flipbook_frame(const ParticleEmitterComponent& e, float age, float life, std::uint32_t start) {
    const std::uint32_t n = flipbook_frame_count(e);
    if (n <= 1) return 0;
    std::uint64_t f = 0;
    if (e.flipbook_fps > 0 && std::isfinite(e.flipbook_fps)) {
        f = static_cast<std::uint64_t>(std::max(0.0f, age) * e.flipbook_fps);
    } else {
        const float t = life > 0 ? std::clamp(age / life, 0.0f, 1.0f) : 0.0f;
        f = std::min<std::uint64_t>(n - 1, static_cast<std::uint64_t>(t * static_cast<float>(n)));
    }
    return static_cast<std::uint32_t>((f + start) % n);
}

void aim_stretched(Billboard& b, const Vec3& velocity, const Mat4& view) {
    if (b.aspect <= 1.0f) return;
    // Screen-space direction of motion: the derivative of the perspective projection of the
    // view-space position p moving with view-space velocity d is proportional to
    // (d.xy * -p.z + p.xy * d.z).
    const Vec3 p = view.transform_point(b.position);
    const Vec3 d = view.transform_vector(velocity);
    float sx = d.x * -p.z + p.x * d.z, sy = d.y * -p.z + p.y * d.z;
    if (sx * sx + sy * sy < 1e-12f) {
        sx = d.x;
        sy = d.y;
    }
    const float speed = math::length(d);
    const float across = std::sqrt(d.x * d.x + d.y * d.y);
    if (speed <= 0.0f || across <= 1e-6f * speed) {
        b.aspect = 1.0f; // moving straight along the view axis: no visible streak
        return;
    }
    b.rotation = std::atan2(-sx, sy);
    b.aspect = 1.0f + (b.aspect - 1.0f) * (across / speed); // foreshortened streak length
}

math::Vec4 flipbook_uv(const Billboard& b) {
    if (b.grid < 17.0f) return {0, 0, 1, 1};
    const auto g = static_cast<std::uint32_t>(b.grid);
    const std::uint32_t cols = std::max<std::uint32_t>(1, g / 16), rows = std::max<std::uint32_t>(1, g % 16);
    const auto f = static_cast<std::uint32_t>(std::max(0.0f, b.frame) + 0.5f) % (cols * rows);
    const float c = static_cast<float>(f % cols), r = static_cast<float>(f / cols);
    const float fc = static_cast<float>(cols), fr = static_cast<float>(rows);
    return {c / fc, r / fr, (c + 1.0f) / fc, (r + 1.0f) / fr};
}

namespace {

// One octave of the curl of psi = (sin(k y + 2ph + 4.1), sin(k z + ph), sin(k x + 3ph + 1.7)):
// every component depends on one other axis only, so the divergence is exactly zero.
Vec3 curl_octave(Vec3 x, float k, float ph) {
    return {-std::cos(k * x.z + ph), -std::cos(k * x.x + 3.0f * ph + 1.7f), -std::cos(k * x.y + 2.0f * ph + 4.1f)};
}

// Orthonormal rotation for the second octave (rows), so the field is not axis aligned.
constexpr float kRot[3][3] = {{0.36f, 0.48f, -0.8f}, {-0.8f, 0.6f, 0.0f}, {0.48f, 0.64f, 0.6f}};

} // namespace

float turbulence_phase(const ParticleEmitterComponent& e, double time) {
    if (!std::isfinite(e.turbulence_speed) || !std::isfinite(time)) return 0.0f;
    return static_cast<float>(std::fmod(time * static_cast<double>(e.turbulence_speed), 2.0 * 3.14159265358979323846));
}

Vec3 turbulence_at(const ParticleEmitterComponent& e, const Vec3& position, float phase) {
    if (e.turbulence == 0.0f || !std::isfinite(e.turbulence)) return {};
    const float k = 2.0f * kPi * std::max(0.0f, e.turbulence_frequency);
    const Vec3 a = curl_octave(position, k, phase);
    const Vec3 r{kRot[0][0] * position.x + kRot[0][1] * position.y + kRot[0][2] * position.z,
                 kRot[1][0] * position.x + kRot[1][1] * position.y + kRot[1][2] * position.z,
                 kRot[2][0] * position.x + kRot[2][1] * position.y + kRot[2][2] * position.z};
    const Vec3 b = curl_octave(r, 2.13f * k, -phase);
    // Back to the original frame with the transpose (the field stays divergence-free).
    const Vec3 bb{kRot[0][0] * b.x + kRot[1][0] * b.y + kRot[2][0] * b.z, kRot[0][1] * b.x + kRot[1][1] * b.y + kRot[2][1] * b.z,
                  kRot[0][2] * b.x + kRot[1][2] * b.y + kRot[2][2] * b.z};
    return (a + bb * 0.5f) * (e.turbulence / 1.5f);
}

void gpu_integrate(const ParticleEmitterComponent& e, GpuParticle& p, float dt, double time) {
    if (!(dt > 0.0f)) return;
    const int steps = std::clamp(static_cast<int>(std::ceil(dt * 30.0f)), 1, 8);
    const float h = dt / static_cast<float>(steps);
    const float damping = e.drag > 0 ? std::exp(-e.drag * h) : 1.0f;
    float phase = turbulence_phase(e, time);
    const float dphase = std::isfinite(e.turbulence_speed) ? e.turbulence_speed * h : 0.0f;
    for (int i = 0; i < steps; ++i) {
        const Vec3 a = e.gravity + turbulence_at(e, p.position, phase);
        p.velocity = (p.velocity + a * h) * damping;
        p.position = p.position + p.velocity * h;
        p.rotation += p.spin * h;
        p.age += h;
        phase += dphase;
    }
}

Billboard gpu_billboard(const ParticleEmitterComponent& e, const GpuParticle& p, const Mat4& world, const Mat4& view) {
    Billboard b;
    if (!(p.age < p.life)) return b;
    const float t = std::clamp(p.age / p.life, 0.0f, 1.0f);
    const float intensity = std::max(0.0f, e.intensity);
    b.position = e.world_space ? p.position : world.transform_point(p.position);
    b.size = std::max(0.0f, math::lerp(e.size_start, e.size_end, t) * p.size_scale);
    b.color = Color(math::lerp(e.color_start.r, e.color_end.r, t) * intensity, math::lerp(e.color_start.g, e.color_end.g, t) * intensity,
                    math::lerp(e.color_start.b, e.color_end.b, t) * intensity, std::clamp(math::lerp(e.color_start.a, e.color_end.a, t), 0.0f, 1.0f));
    b.rotation = p.rotation;
    const std::uint32_t cols = std::clamp<std::uint32_t>(e.flipbook_columns, 1, 15), rows = std::clamp<std::uint32_t>(e.flipbook_rows, 1, 15);
    if (cols * rows > 1) {
        b.grid = static_cast<float>(cols * 16 + rows);
        b.frame = static_cast<float>(flipbook_frame(e, p.age, p.life, static_cast<std::uint32_t>(p.start_frame)));
    }
    if (e.velocity_stretch > 0 && std::isfinite(e.velocity_stretch)) {
        const Vec3 v = e.world_space ? p.velocity : world.transform_vector(p.velocity);
        const float speed = math::length(v);
        if (b.size > 0 && speed > 1e-6f) {
            b.aspect = 1.0f + speed * e.velocity_stretch / b.size;
            b.rotation = std::atan2(-v.x, v.y);
            aim_stretched(b, v, view);
        }
    }
    return b;
}

namespace {

struct Counts {
    std::uint32_t spawned{0}, died{0};
};

std::uint32_t trail_capacity(const ParticleEmitterComponent& e) {
    if (!(e.trail_lifetime > 0) || !std::isfinite(e.trail_lifetime)) return 0;
    return std::clamp<std::uint32_t>(e.trail_max_points, 2, 64);
}

// Particles due this update (pending + bursts + rate); advances the cycle clock. `active` is
// false once a non-looping cycle has ended.
std::uint32_t schedule(const ParticleEmitterComponent& e, EmitterState& s, float dt, bool& active, std::uint32_t limit) {
    std::uint32_t spawn = s.pending;
    s.pending = 0;
    active = true;
    if (!e.emitting) return spawn;
    if (!s.started) {
        s.started = true;
        s.time = 0;
        spawn += e.burst;
    } else {
        s.time += dt;
    }
    if (e.duration > 0 && s.time >= e.duration) {
        if (e.loop) {
            s.time = std::fmod(s.time, e.duration);
            spawn += e.burst;
        } else {
            active = false;
        }
    }
    if (active && e.rate > 0) {
        s.accumulator += e.rate * dt;
        const float whole = std::floor(s.accumulator);
        s.accumulator -= whole;
        spawn += static_cast<std::uint32_t>(std::min(whole, static_cast<float>(limit)));
    } else {
        s.accumulator = 0;
    }
    return std::min(spawn, limit);
}

// Appends the particle's position when it moved far enough (dropping the oldest point when the
// block is full), then expires points older than the trail lifetime.
void record_trail(const ParticleEmitterComponent& e, Particle& p, TrailPoint* block, std::uint32_t cap) {
    const float min_d = std::max(0.0f, e.trail_min_distance);
    if (p.trail_count == 0 || math::length(p.position - block[p.trail_count - 1].position) >= min_d) {
        if (p.trail_count == cap) {
            std::copy(block + 1, block + cap, block);
            --p.trail_count;
        }
        block[p.trail_count++] = {p.position, p.age};
    }
    std::uint32_t drop = 0;
    while (drop < p.trail_count && p.age - block[drop].age > e.trail_lifetime) ++drop;
    if (drop > 0) {
        std::copy(block + drop, block + p.trail_count, block);
        p.trail_count -= drop;
    }
}

Counts simulate(const ParticleEmitterComponent& e, EmitterState& s, const Mat4& world, float dt, std::uint64_t uuid_seed) {
    Counts counts;
    if (!s.seeded) {
        const std::uint64_t seed = e.seed != 0 ? e.seed : uuid_seed;
        s.rng = Random(seed, 0x9e3779b97f4a7c15ull ^ seed);
        s.seeded = true;
    }
    dt = std::max(0.0f, dt);
    // Integrate and retire existing particles (swap-remove: order is irrelevant, the renderer sorts).
    const float damping = e.drag > 0 ? std::exp(-e.drag * dt) : 1.0f;
    const bool turbulent = e.turbulence != 0.0f && std::isfinite(e.turbulence);
    const float phase = turbulent ? turbulence_phase(e, s.clock) : 0.0f;
    s.clock += dt;
    const std::uint32_t cap = trail_capacity(e);
    if (cap != s.trail_cap) { // trails switched on/off or resized: restart every history
        s.trail_cap = cap;
        for (Particle& p : s.particles) p.trail_count = 0;
    }
    s.trail.resize(s.particles.size() * cap);
    s.events.clear();
    const bool sub = has_sub_emitter(e);
    const bool sub_death = sub && e.sub_emitter_trigger == SubEmitterTrigger::Death;
    for (std::size_t i = 0; i < s.particles.size();) {
        Particle& p = s.particles[i];
        p.age += dt;
        if (p.age >= p.life) {
            if (sub_death) {
                // Where the particle would be at the end of the step it dies in.
                const Vec3 at = p.position + p.velocity * dt;
                push_event(e, s, e.world_space ? at : world.transform_point(at), e.world_space ? p.velocity : world.transform_vector(p.velocity));
            }
            const std::size_t last = s.particles.size() - 1;
            if (cap > 0 && i != last) std::copy_n(s.trail.begin() + static_cast<std::ptrdiff_t>(last * cap), cap, s.trail.begin() + static_cast<std::ptrdiff_t>(i * cap));
            p = s.particles.back();
            s.particles.pop_back();
            ++counts.died;
            continue;
        }
        const Vec3 accel = turbulent ? e.gravity + turbulence_at(e, p.position, phase) : e.gravity;
        p.velocity = (p.velocity + accel * dt) * damping;
        p.position = p.position + p.velocity * dt;
        p.rotation += p.spin * dt;
        if (cap > 0) record_trail(e, p, s.trail.data() + i * cap, cap);
        ++i;
    }

    // Emission.
    bool active = true;
    std::uint32_t spawn = schedule(e, s, dt, active, kHardCap);
    if (e.emitting) s.finished = !active && s.particles.empty() && spawn == 0;
    const std::uint32_t max_alive = std::min(e.max_particles, kHardCap);
    const std::uint32_t room = max_alive > s.particles.size() ? max_alive - static_cast<std::uint32_t>(s.particles.size()) : 0;
    spawn = std::min(spawn, room);
    std::uint32_t sub_spawn = 0; // particles requested by sources' events, within the remaining room
    for (const SubEvent& ev : s.incoming) sub_spawn = std::min(room - spawn, sub_spawn + ev.count);
    const float life_lo = std::max(0.001f, std::min(e.lifetime_min, e.lifetime_max));
    const float life_hi = std::max(life_lo, std::max(e.lifetime_min, e.lifetime_max));
    const float spin_to_rad = kPi / 180.0f;
    s.trail.reserve((s.particles.size() + spawn + sub_spawn) * cap);
    std::size_t event = 0;
    std::uint32_t event_used = 0;
    for (std::uint32_t i = 0; i < spawn + sub_spawn; ++i) {
        const SubEvent* ev = nullptr;
        if (i >= spawn) { // sub-emitter spawns follow the regular ones, event by event
            while (event_used >= s.incoming[event].count) {
                ++event;
                event_used = 0;
            }
            ev = &s.incoming[event];
            ++event_used;
        }
        Vec3 pos, dir;
        sample_shape(e, s.rng, pos, dir);
        const float speed = s.rng.range_float(std::min(e.speed_min, e.speed_max), std::max(e.speed_min, e.speed_max));
        Particle p;
        p.life = s.rng.range_float(life_lo, life_hi);
        p.rotation = s.rng.next_float() * 2.0f * kPi;
        p.spin = s.rng.range_float(e.spin_min, e.spin_max) * spin_to_rad;
        p.size_scale = 1.0f - clamp01(e.size_variation) * s.rng.next_float();
        // Drawn only when used, so emitters without the option keep their exact random sequence.
        if (e.flipbook_random_start) {
            const std::uint32_t n = flipbook_frame_count(e);
            if (n > 1) p.start_frame = std::min(n - 1, static_cast<std::uint32_t>(s.rng.next_float() * static_cast<float>(n)));
        }
        if (ev != nullptr) {
            place_sub(e, world, *ev, pos, dir, speed, p.position, p.velocity);
        } else if (e.world_space) {
            p.position = world.transform_point(pos);
            p.velocity = math::normalize(world.transform_vector(dir)) * speed;
        } else {
            p.position = pos;
            p.velocity = dir * speed;
        }
        if (sub && e.sub_emitter_trigger == SubEmitterTrigger::Birth)
            push_event(e, s, e.world_space ? p.position : world.transform_point(p.position), e.world_space ? p.velocity : world.transform_vector(p.velocity));
        s.particles.push_back(p);
        if (cap > 0) {
            s.trail.resize(s.particles.size() * cap);
            record_trail(e, s.particles.back(), s.trail.data() + (s.particles.size() - 1) * cap, cap);
        }
    }
    s.trail.resize(s.particles.size() * cap);
    s.incoming.clear();
    spawn += sub_spawn;
    counts.spawned = spawn;
    s.emitted += spawn;

    // Billboards in world space.
    s.billboards.resize(s.particles.size());
    const bool stretched = e.velocity_stretch > 0 && std::isfinite(e.velocity_stretch);
    s.velocities.resize(stretched ? s.particles.size() : 0);
    s.bounds = {};
    const float intensity = std::max(0.0f, e.intensity);
    const std::uint32_t cols = grid_axis(e.flipbook_columns), rows = grid_axis(e.flipbook_rows);
    const bool flipbook = cols * rows > 1;
    const float grid = flipbook ? static_cast<float>(cols * 16 + rows) : 0.0f;
    for (std::size_t i = 0; i < s.particles.size(); ++i) {
        const Particle& p = s.particles[i];
        const float t = clamp01(p.age / p.life);
        Billboard& b = s.billboards[i];
        b.position = e.world_space ? p.position : world.transform_point(p.position);
        b.size = std::max(0.0f, math::lerp(e.size_start, e.size_end, t) * p.size_scale);
        b.color = Color(math::lerp(e.color_start.r, e.color_end.r, t) * intensity, math::lerp(e.color_start.g, e.color_end.g, t) * intensity,
                        math::lerp(e.color_start.b, e.color_end.b, t) * intensity, clamp01(math::lerp(e.color_start.a, e.color_end.a, t)));
        b.rotation = p.rotation;
        b.frame = flipbook ? static_cast<float>(flipbook_frame(e, p.age, p.life, p.start_frame)) : 0.0f;
        b.grid = grid;
        b.aspect = 0;
        float extent = b.size;
        if (stretched) {
            const Vec3 v = e.world_space ? p.velocity : world.transform_vector(p.velocity);
            s.velocities[i] = v;
            const float speed = math::length(v);
            if (b.size > 0 && speed > 1e-6f) {
                b.aspect = 1.0f + speed * e.velocity_stretch / b.size;
                // Up axis along the velocity's XY direction: right for 2D and for a camera looking
                // down -Z; 3D views re-aim it at the camera with velocities.
                b.rotation = std::atan2(-v.x, v.y);
                extent = b.size * b.aspect;
            }
        }
        const Vec3 half{extent * 0.5f};
        s.bounds.expand(b.position - half);
        s.bounds.expand(b.position + half);
    }

    // Trails: recorded points from tail to head, then the particle's current position.
    s.ribbon_points.clear();
    s.ribbon_strips.clear();
    if (cap > 0) {
        const float head_w = std::max(0.0f, e.trail_width), end_w = std::max(0.0f, e.trail_width_end);
        const float end_a = clamp01(e.trail_alpha_end);
        for (std::size_t i = 0; i < s.particles.size(); ++i) {
            const Particle& p = s.particles[i];
            const Billboard& b = s.billboards[i];
            const TrailPoint* block = s.trail.data() + i * cap;
            const auto first = static_cast<std::uint32_t>(s.ribbon_points.size());
            const auto add = [&](const Vec3& sim_pos, float point_age) {
                const float v = clamp01((p.age - point_age) / e.trail_lifetime);
                RibbonPoint r;
                r.position = e.world_space ? sim_pos : world.transform_point(sim_pos);
                r.width = b.size * head_w * math::lerp(1.0f, end_w, v);
                r.color = Color(b.color.r, b.color.g, b.color.b, b.color.a * math::lerp(1.0f, end_a, v));
                r.v = v;
                s.ribbon_points.push_back(r);
                const Vec3 half{r.width * 0.5f};
                s.bounds.expand(r.position - half);
                s.bounds.expand(r.position + half);
            };
            for (std::uint32_t k = 0; k < p.trail_count; ++k) add(block[k].position, block[k].age);
            // The newest recorded point may be the current position (just recorded); skip duplicates.
            if (p.trail_count == 0 || math::length(p.position - block[p.trail_count - 1].position) > 1e-6f) add(p.position, p.age);
            const auto n = static_cast<std::uint32_t>(s.ribbon_points.size()) - first;
            if (n >= 2) s.ribbon_strips.push_back(n);
            else s.ribbon_points.resize(first);
        }
    }
    if (!e.trail_billboards && cap > 0) {
        // Trails only: keep the bounds of the trails, drop the heads.
        s.billboards.clear();
        s.velocities.clear();
        s.bounds = {};
        for (const RibbonPoint& r : s.ribbon_points) {
            const Vec3 half{r.width * 0.5f};
            s.bounds.expand(r.position - half);
            s.bounds.expand(r.position + half);
        }
    }
    return counts;
}

void reset_book(GpuBook& g, std::uint32_t capacity) {
    g.capacity = capacity;
    ++g.generation;
    g.live.clear();
    g.free.resize(capacity);
    for (std::uint32_t i = 0; i < capacity; ++i) g.free[i] = capacity - 1 - i; // slot 0 on top
    g.records.clear();
    g.head = 0;
    g.boxes.clear();
}

using DeathHeap = std::greater<std::pair<double, std::uint32_t>>;

// Conservative world bounds of everything a GPU emitter can have alive: the boxes of spawn
// positions still referenced, grown by the farthest a particle can travel in its longest life
// (speed plus constant acceleration, ignoring drag) and its largest drawn extent.
Aabb gpu_bounds(const ParticleEmitterComponent& e, const GpuBook& g, const Mat4& world, float life_hi) {
    Aabb box;
    for (const auto& b : g.boxes) {
        box.expand(b.second.min);
        box.expand(b.second.max);
    }
    if (!box.valid()) return box;
    const float speed = std::max(std::fabs(e.speed_min), std::fabs(e.speed_max));
    const float accel = math::length(e.gravity) + (std::isfinite(e.turbulence) ? std::fabs(e.turbulence) : 0.0f);
    const float travel = speed * life_hi + 0.5f * accel * life_hi * life_hi;
    float extent = std::max(e.size_start, e.size_end) * 0.5f;
    if (e.velocity_stretch > 0 && std::isfinite(e.velocity_stretch)) extent += (speed + accel * life_hi) * e.velocity_stretch;
    // Trails (ADR-0110) lie on positions the particles already passed, inside the travel box; only
    // their width can reach further out.
    if (trail_capacity(e) > 0) extent = std::max(extent, std::max(e.size_start, e.size_end) * 0.5f * std::max(0.0f, e.trail_width));
    const Vec3 grow{travel + extent};
    box.min = box.min - grow;
    box.max = box.max + grow;
    return e.world_space ? box : box.transformed(world);
}

Counts simulate_gpu(const ParticleEmitterComponent& e, EmitterState& s, const Mat4& world, float dt, std::uint64_t uuid_seed) {
    Counts counts;
    GpuBook& g = s.book;
    if (!s.seeded) {
        const std::uint64_t seed = e.seed != 0 ? e.seed : uuid_seed;
        s.rng = Random(seed, 0x9e3779b97f4a7c15ull ^ seed);
        s.seeded = true;
    }
    if (g.instance == 0) g.instance = g_instances.fetch_add(1, std::memory_order_relaxed);
    dt = std::max(0.0f, std::isfinite(dt) ? dt : 0.0f);
    const std::uint32_t capacity = std::clamp<std::uint32_t>(e.max_particles, 1, kMaxGpuParticles);
    if (capacity != g.capacity) reset_book(g, capacity);
    const double t0 = g.time, t1 = t0 + static_cast<double>(dt);
    g.time = t1;
    s.clock = t1;
    // Retire every particle whose life ends by the end of this step.
    while (!g.live.empty() && g.live.front().first <= t1) {
        std::pop_heap(g.live.begin(), g.live.end(), DeathHeap{});
        g.free.push_back(g.live.back().second);
        g.live.pop_back();
        ++counts.died;
    }
    bool active = true;
    std::uint32_t spawn = schedule(e, s, dt, active, kMaxGpuParticles);
    spawn = std::min<std::uint32_t>(spawn, static_cast<std::uint32_t>(g.free.size()));
    std::uint32_t sub_spawn = 0;
    const auto free_after = static_cast<std::uint32_t>(g.free.size()) - spawn;
    for (const SubEvent& ev : s.incoming) sub_spawn = std::min(free_after, sub_spawn + ev.count);
    s.events.clear(); // GPU emitters cannot be sources: particle positions live on the GPU
    const float life_lo = std::max(0.001f, std::min(e.lifetime_min, e.lifetime_max));
    const float life_hi = std::max(life_lo, std::max(e.lifetime_min, e.lifetime_max));
    const float spin_to_rad = kPi / 180.0f;
    const Mat4 prev = g.has_prev ? g.prev_world : world;
    Aabb spawn_box;
    double last_death = t1;
    std::size_t event = 0;
    std::uint32_t event_used = 0;
    for (std::uint32_t i = 0; i < spawn + sub_spawn; ++i) {
        const SubEvent* ev = nullptr;
        if (i >= spawn) { // sub-emitter spawns start at the beginning of the step, at the event
            while (event_used >= s.incoming[event].count) {
                ++event;
                event_used = 0;
            }
            ev = &s.incoming[event];
            ++event_used;
        }
        // Spread the step's spawns over its duration; a moving emitter leaves an even stream.
        const float frac = ev != nullptr ? 0.0f : static_cast<float>(i + 1) / static_cast<float>(spawn);
        const double ts = t0 + static_cast<double>(dt) * static_cast<double>(frac);
        Mat4 w = world;
        w[3] = math::Vec4(math::lerp(prev[3].x, world[3].x, frac), math::lerp(prev[3].y, world[3].y, frac),
                          math::lerp(prev[3].z, world[3].z, frac), 1.0f);
        Vec3 pos, dir;
        sample_shape(e, s.rng, pos, dir);
        const float speed = s.rng.range_float(std::min(e.speed_min, e.speed_max), std::max(e.speed_min, e.speed_max));
        GpuParticle p;
        p.life = s.rng.range_float(life_lo, life_hi);
        p.rotation = s.rng.next_float() * 2.0f * kPi;
        p.spin = s.rng.range_float(e.spin_min, e.spin_max) * spin_to_rad;
        p.size_scale = 1.0f - clamp01(e.size_variation) * s.rng.next_float();
        if (e.flipbook_random_start) {
            const std::uint32_t n = flipbook_frame_count(e);
            if (n > 1) p.start_frame = static_cast<float>(std::min(n - 1, static_cast<std::uint32_t>(s.rng.next_float() * static_cast<float>(n))));
        }
        if (ev != nullptr) {
            Vec3 pp, pv;
            place_sub(e, world, *ev, pos, dir, speed, pp, pv);
            p.position = pp;
            p.velocity = pv;
        } else if (e.world_space) {
            p.position = w.transform_point(pos);
            p.velocity = math::normalize(w.transform_vector(dir)) * speed;
        } else {
            p.position = pos;
            p.velocity = dir * speed;
        }
        const std::uint32_t slot = g.free.back();
        g.free.pop_back();
        p.meta[0] = slot;
        const double death = ts + static_cast<double>(p.life);
        last_death = std::max(last_death, death);
        g.live.emplace_back(death, slot);
        std::push_heap(g.live.begin(), g.live.end(), DeathHeap{});
        g.records.push_back({p, g.spawned++, ts});
        spawn_box.expand(p.position);
    }
    s.incoming.clear();
    spawn += sub_spawn;
    if (spawn > 0) g.boxes.emplace_back(last_death, spawn_box);
    while (!g.boxes.empty() && g.boxes.front().first <= t1) g.boxes.pop_front();
    // Keep records of particles that may still be alive, bounded; compact the vector lazily.
    while (g.head < g.records.size() &&
           (g.records[g.head].time + static_cast<double>(g.records[g.head].particle.life) <= t1 || g.records.size() - g.head > kMaxRetainedRecords))
        ++g.head;
    if (g.head > 0 && g.head * 2 >= g.records.size()) {
        g.records.erase(g.records.begin(), g.records.begin() + static_cast<std::ptrdiff_t>(g.head));
        g.head = 0;
    }
    g.prev_world = world;
    g.has_prev = true;
    counts.spawned = spawn;
    s.emitted += spawn;
    if (e.emitting) s.finished = !active && g.live.empty() && spawn == 0;
    s.bounds = gpu_bounds(e, g, world, life_hi);
    GpuEmitterView& v = g.view;
    v.key = uuid_seed;
    v.instance = g.instance;
    v.generation = g.generation;
    v.capacity = g.capacity;
    v.alive = static_cast<std::uint32_t>(g.live.size());
    v.time = g.time;
    v.spawned = g.spawned;
    v.records = std::span<const GpuSpawnRecord>(g.records).subspan(g.head);
    v.world = world;
    return counts;
}

// Switching paths drops the other path's particles.
void switch_path(EmitterState& s, bool gpu) {
    if (s.gpu == gpu) return;
    s.gpu = gpu;
    s.particles.clear();
    s.billboards.clear();
    s.velocities.clear();
    s.trail.clear();
    s.ribbon_points.clear();
    s.ribbon_strips.clear();
    s.bounds = {};
    if (s.book.capacity > 0) reset_book(s.book, s.book.capacity);
}

std::uint64_t seed_of(const Uuid& u, Entity e) {
    const std::uint64_t base = u.is_nil() ? (static_cast<std::uint64_t>(e.index) + 1) * 0x2545f4914f6cdd1dull : u.hi ^ (u.lo * 0x9e3779b97f4a7c15ull);
    return base == 0 ? 1 : base;
}

} // namespace

struct ParticleSystem::Work {
    Entity entity;
    const ParticleEmitterComponent* emitter{nullptr};
    EmitterState* state{nullptr};
    Mat4 world;
    std::uint64_t seed{0};
    Counts counts;
};

void register_components() {
    static std::once_flag once;
    std::call_once(once, [] {
        scene::Schema<ParticleEmitterComponent>("ParticleEmitter", "Effects")
            .field(&ParticleEmitterComponent::emitting, "emitting")
            .field(&ParticleEmitterComponent::loop, "loop")
            .field(&ParticleEmitterComponent::world_space, "world_space", {.tooltip = "Off: particles follow the entity"})
            .field(&ParticleEmitterComponent::duration, "duration", {.min = 0, .max = 1e4f, .speed = 0.05f, .tooltip = "Cycle length in seconds; 0 = forever"})
            .field(&ParticleEmitterComponent::rate, "rate", {.min = 0, .max = 1e5f, .speed = 0.5f, .tooltip = "particles per second"})
            .field(&ParticleEmitterComponent::burst, "burst", {.min = 0, .max = 65536, .tooltip = "Particles at the start of each cycle"})
            .field(&ParticleEmitterComponent::max_particles, "max_particles", {.min = 1, .max = static_cast<float>(kMaxGpuParticles), .tooltip = "CPU emitters stop at 65536"})
            .field(&ParticleEmitterComponent::lifetime_min, "lifetime_min", {.min = 0.001f, .max = 1e3f, .speed = 0.01f, .tooltip = "seconds"})
            .field(&ParticleEmitterComponent::lifetime_max, "lifetime_max", {.min = 0.001f, .max = 1e3f, .speed = 0.01f, .tooltip = "seconds"})
            .enumeration(&ParticleEmitterComponent::shape, "shape", {"point", "sphere", "cone", "box"})
            .field(&ParticleEmitterComponent::radius, "radius", {.min = 0, .max = 1e3f, .speed = 0.01f})
            .field(&ParticleEmitterComponent::angle, "angle", {.min = 0, .max = 180, .speed = 0.5f, .hint = "degrees"})
            .field(&ParticleEmitterComponent::box, "box", {.speed = 0.05f})
            .field(&ParticleEmitterComponent::speed_min, "speed_min", {.min = -1e3f, .max = 1e3f, .speed = 0.05f})
            .field(&ParticleEmitterComponent::speed_max, "speed_max", {.min = -1e3f, .max = 1e3f, .speed = 0.05f})
            .field(&ParticleEmitterComponent::gravity, "gravity", {.speed = 0.05f, .tooltip = "m/s^2"})
            .field(&ParticleEmitterComponent::drag, "drag", {.min = 0, .max = 100, .speed = 0.01f})
            .field(&ParticleEmitterComponent::size_start, "size_start", {.min = 0, .max = 1e3f, .speed = 0.005f})
            .field(&ParticleEmitterComponent::size_end, "size_end", {.min = 0, .max = 1e3f, .speed = 0.005f})
            .field(&ParticleEmitterComponent::size_variation, "size_variation", {.min = 0, .max = 1, .speed = 0.01f})
            .field(&ParticleEmitterComponent::color_start, "color_start")
            .field(&ParticleEmitterComponent::color_end, "color_end")
            .field(&ParticleEmitterComponent::intensity, "intensity", {.min = 0, .max = 1e3f, .speed = 0.05f, .tooltip = "> 1 glows through bloom"})
            .field(&ParticleEmitterComponent::spin_min, "spin_min", {.min = -1e4f, .max = 1e4f, .speed = 1, .hint = "degrees"})
            .field(&ParticleEmitterComponent::spin_max, "spin_max", {.min = -1e4f, .max = 1e4f, .speed = 1, .hint = "degrees"})
            .enumeration(&ParticleEmitterComponent::blend, "blend", {"alpha", "additive"})
            .field(&ParticleEmitterComponent::texture, "texture", {.tooltip = "Optional texture; empty = soft disc", .hint = "texture"})
            .field(&ParticleEmitterComponent::softness, "softness", {.min = 0, .max = 100, .speed = 0.01f, .tooltip = "metres"})
            .field(&ParticleEmitterComponent::layer, "layer", {.min = -1000, .max = 1000, .tooltip = "2D sprite layer"})
            .field(&ParticleEmitterComponent::seed, "seed", {.tooltip = "0 = derived from the entity"})
            .field(&ParticleEmitterComponent::flipbook_columns, "flipbook_columns", {.min = 1, .max = 15, .tooltip = "Texture frame grid columns"})
            .field(&ParticleEmitterComponent::flipbook_rows, "flipbook_rows", {.min = 1, .max = 15, .tooltip = "Texture frame grid rows"})
            .field(&ParticleEmitterComponent::flipbook_frames, "flipbook_frames", {.min = 0, .max = 225, .tooltip = "Frames used; 0 = columns x rows"})
            .field(&ParticleEmitterComponent::flipbook_fps, "flipbook_fps", {.min = 0, .max = 240, .speed = 0.5f, .tooltip = "0 = frames span each particle's life"})
            .field(&ParticleEmitterComponent::flipbook_random_start, "flipbook_random_start")
            .field(&ParticleEmitterComponent::velocity_stretch, "velocity_stretch", {.min = 0, .max = 10, .speed = 0.005f, .tooltip = "Seconds of motion added to the length; 0 = off"})
            .field(&ParticleEmitterComponent::trail_lifetime, "trail_lifetime", {.min = 0, .max = 60, .speed = 0.01f, .tooltip = "Seconds a trail point lives; 0 = no trails"})
            .field(&ParticleEmitterComponent::trail_min_distance, "trail_min_distance", {.min = 0, .max = 100, .speed = 0.005f, .tooltip = "Metres moved before a new trail point"})
            .field(&ParticleEmitterComponent::trail_max_points, "trail_max_points", {.min = 2, .max = 64, .tooltip = "Recorded points per particle"})
            .field(&ParticleEmitterComponent::trail_width, "trail_width", {.min = 0, .max = 100, .speed = 0.01f, .tooltip = "Head width x particle size"})
            .field(&ParticleEmitterComponent::trail_width_end, "trail_width_end", {.min = 0, .max = 10, .speed = 0.01f, .tooltip = "Tail width as a fraction of the head"})
            .field(&ParticleEmitterComponent::trail_alpha_end, "trail_alpha_end", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "Tail alpha as a fraction of the head"})
            .field(&ParticleEmitterComponent::trail_billboards, "trail_billboards", {.tooltip = "Also draw the particles"})
            .field(&ParticleEmitterComponent::turbulence, "turbulence", {.min = 0, .max = 1000, .speed = 0.05f, .tooltip = "Swirling curl-noise acceleration (m/s^2); 0 = off"})
            .field(&ParticleEmitterComponent::turbulence_frequency, "turbulence_frequency", {.min = 0, .max = 100, .speed = 0.01f, .tooltip = "Swirls per metre"})
            .field(&ParticleEmitterComponent::turbulence_speed, "turbulence_speed", {.min = 0, .max = 100, .speed = 0.01f, .tooltip = "How fast the swirls change"})
            .field(&ParticleEmitterComponent::gpu, "gpu", {.tooltip = "Simulate on the GPU (3D): up to a million particles, depth collision, sorting and trails"})
            .field(&ParticleEmitterComponent::collision, "collision", {.tooltip = "GPU, world space: bounce off what the camera sees"})
            .field(&ParticleEmitterComponent::bounce, "bounce", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "0 = stop, 1 = elastic"})
            .field(&ParticleEmitterComponent::friction, "friction", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "Tangential speed lost per contact"})
            .field(&ParticleEmitterComponent::collision_thickness, "collision_thickness", {.min = 0.01f, .max = 100, .speed = 0.01f, .tooltip = "Metres behind a visible surface treated as solid"})
            .field(&ParticleEmitterComponent::sub_emitter, "sub_emitter", {.tooltip = "Emitter that bursts where this emitter's particles die or are born (CPU emitters)", .hint = "entity"})
            .enumeration(&ParticleEmitterComponent::sub_emitter_trigger, "sub_emitter_trigger", {"death", "birth"})
            .field(&ParticleEmitterComponent::sub_emitter_count, "sub_emitter_count", {.min = 0, .max = 65536, .tooltip = "Particles per birth or death"})
            .field(&ParticleEmitterComponent::sub_emitter_inherit_velocity, "sub_emitter_inherit_velocity", {.min = -10, .max = 10, .speed = 0.01f, .tooltip = "Fraction of the particle's velocity passed on"})
            .commit();
    });
}

ParticleSystem::ParticleSystem(ParticleSettings settings) : settings_(settings) { register_components(); }
ParticleSystem::~ParticleSystem() = default;

void ParticleSystem::update(Scene& scene, float dt) {
    ecs::World& world = scene.world();
    std::vector<Entity> missing, orphans;
    world.each<ParticleEmitterComponent>([&](Entity e, ParticleEmitterComponent&) {
        if (!world.has<EmitterState>(e)) missing.push_back(e);
    });
    world.each_excluding<EmitterState>(ecs::World::Exclude<ParticleEmitterComponent>{}, [&](Entity e, EmitterState&) { orphans.push_back(e); });
    for (const Entity e : missing) world.add<EmitterState>(e);
    for (const Entity e : orphans) world.remove<EmitterState>(e);

    work_.clear();
    world.each<ParticleEmitterComponent, EmitterState>([&](Entity e, ParticleEmitterComponent& c, EmitterState& s) {
        work_.push_back({e, &c, &s, scene.world_matrix(e), seed_of(scene.uuid(e), e), {}});
    });
    const auto run = [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            Work& w = work_[i];
            switch_path(*w.state, w.emitter->gpu);
            w.counts = w.emitter->gpu ? simulate_gpu(*w.emitter, *w.state, w.world, dt, w.seed)
                                      : simulate(*w.emitter, *w.state, w.world, dt, w.seed);
        }
    };
    if (settings_.jobs && work_.size() >= std::max<std::uint32_t>(2, settings_.parallel_threshold)) settings_.jobs->parallel_for(0, work_.size(), 1, run);
    else run(0, work_.size());

    stats_ = {};
    stats_.emitters = static_cast<std::uint32_t>(work_.size());
    for (const Work& w : work_) {
        const auto alive = w.state->gpu ? static_cast<std::uint32_t>(w.state->book.live.size())
                                        : static_cast<std::uint32_t>(w.state->particles.size());
        stats_.alive += alive;
        if (w.state->gpu) {
            ++stats_.gpu_emitters;
            stats_.gpu_alive += alive;
        }
        stats_.spawned += w.counts.spawned;
        stats_.died += w.counts.died;
    }
    // Deliver sub-emitter events serially in work order, so the result does not depend on jobs.
    for (Work& w : work_) {
        std::vector<SubEvent>& events = w.state->events;
        if (events.empty()) continue;
        const Entity target = scene.find(w.emitter->sub_emitter);
        EmitterState* ts = target != w.entity && world.has<ParticleEmitterComponent>(target) ? world.get<EmitterState>(target) : nullptr;
        const auto n = static_cast<std::uint32_t>(events.size());
        if (ts == nullptr) {
            stats_.sub_emitter_dropped += n;
        } else {
            const std::size_t room = ts->incoming.size() < kMaxSubEmitterEvents ? kMaxSubEmitterEvents - ts->incoming.size() : 0;
            const std::size_t take = std::min<std::size_t>(room, events.size());
            ts->incoming.insert(ts->incoming.end(), events.begin(), events.begin() + static_cast<std::ptrdiff_t>(take));
            stats_.sub_emitter_events += static_cast<std::uint32_t>(take);
            stats_.sub_emitter_dropped += n - static_cast<std::uint32_t>(take);
        }
        events.clear();
    }
}

void add_particle_systems(gameplay::Simulation& sim, ParticleSystem& system) {
    ParticleSystem* sys = &system;
    sim.add_system("particles.update", gameplay::Phase::PostUpdate,
                   [sys](Scene& scene, const gameplay::FrameTime& t) { sys->update(scene, static_cast<float>(t.dt)); });
}

bool emit(Scene& scene, Entity e, std::uint32_t count) {
    ecs::World& world = scene.world();
    if (!world.has<ParticleEmitterComponent>(e)) return false;
    if (!world.has<EmitterState>(e)) world.add<EmitterState>(e);
    auto* s = world.get<EmitterState>(e);
    s->pending = std::min(kHardCap, s->pending + std::min(count, kHardCap));
    return true;
}

bool reset(Scene& scene, Entity e) {
    ecs::World& world = scene.world();
    if (!world.has<ParticleEmitterComponent>(e)) return false;
    if (auto* s = world.get<EmitterState>(e)) {
        s->particles.clear();
        s->billboards.clear();
        s->velocities.clear();
        s->trail.clear();
        s->ribbon_points.clear();
        s->ribbon_strips.clear();
        s->bounds = {};
        s->started = false;
        s->finished = false;
        s->time = 0;
        s->accumulator = 0;
        s->pending = 0;
        s->events.clear();
        s->incoming.clear();
        if (s->book.capacity > 0) reset_book(s->book, s->book.capacity);
        s->book.view.generation = s->book.generation;
        s->book.view.alive = 0;
        s->book.view.records = {};
    }
    return true;
}

EmitterStatus status(Scene& scene, Entity e) {
    EmitterStatus out;
    ecs::World& world = scene.world();
    if (!world.has<ParticleEmitterComponent>(e)) return out;
    out.valid = true;
    if (const auto* s = world.get<EmitterState>(e)) {
        out.alive = s->gpu ? static_cast<std::uint32_t>(s->book.live.size()) : static_cast<std::uint32_t>(s->particles.size());
        out.emitted = s->emitted;
        out.time = s->time;
        out.finished = s->finished;
    }
    return out;
}

void gather(Scene& scene, std::vector<EmitterView>& out) {
    scene.world().each<ParticleEmitterComponent, EmitterState>([&](Entity e, ParticleEmitterComponent& c, EmitterState& s) {
        if (s.gpu) {
            // Listed even when empty so a renderer sees resets and can drop its GPU particles.
            if (s.book.capacity == 0) return;
            EmitterView v{e, &c, {}, {}, s.bounds, {}, {}};
            v.gpu = &s.book.view;
            out.push_back(v);
            return;
        }
        if (s.billboards.empty() && s.ribbon_strips.empty()) return;
        out.push_back({e, &c, std::span<const Billboard>(s.billboards), std::span<const Vec3>(s.velocities), s.bounds,
                       std::span<const RibbonPoint>(s.ribbon_points), std::span<const std::uint32_t>(s.ribbon_strips)});
    });
}

} // namespace victus::particles
