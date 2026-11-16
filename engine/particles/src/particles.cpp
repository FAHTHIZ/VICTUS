// SPDX-License-Identifier: Apache-2.0
#include "victus/particles/particles.hpp"

#include "victus/core/jobs.hpp"
#include "victus/scene/reflect.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace victus::particles {

using math::Aabb;
using math::Color;
using math::Mat4;
using math::Vec3;
using scene::Entity;
using scene::Scene;

namespace {

constexpr std::uint32_t kHardCap = 65536;
constexpr float kPi = 3.14159265358979f;

struct Particle {
    Vec3 position;
    float age{0};
    Vec3 velocity;
    float life{1};
    float rotation{0};
    float spin{0};
    float size_scale{1};
};

// Runtime state, not serialized: rebuilt (empty) after loading.
struct EmitterState {
    std::vector<Particle> particles;
    std::vector<Billboard> billboards;
    Aabb bounds;
    Random rng{1, 1};
    bool seeded{false};
    bool started{false};
    bool finished{false};
    float time{0};
    float accumulator{0};
    std::uint32_t pending{0};
    std::uint64_t emitted{0};
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

struct Counts {
    std::uint32_t spawned{0}, died{0};
};

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
    for (std::size_t i = 0; i < s.particles.size();) {
        Particle& p = s.particles[i];
        p.age += dt;
        if (p.age >= p.life) {
            p = s.particles.back();
            s.particles.pop_back();
            ++counts.died;
            continue;
        }
        p.velocity = (p.velocity + e.gravity * dt) * damping;
        p.position = p.position + p.velocity * dt;
        p.rotation += p.spin * dt;
        ++i;
    }

    // Emission.
    std::uint32_t spawn = s.pending;
    s.pending = 0;
    if (e.emitting) {
        if (!s.started) {
            s.started = true;
            s.time = 0;
            spawn += e.burst;
        } else {
            s.time += dt;
        }
        bool active = true;
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
            spawn += static_cast<std::uint32_t>(std::min(whole, static_cast<float>(kHardCap)));
        } else {
            s.accumulator = 0;
        }
        s.finished = !active && s.particles.empty() && spawn == 0;
    }
    const std::uint32_t cap = std::min(e.max_particles, kHardCap);
    const std::uint32_t room = cap > s.particles.size() ? cap - static_cast<std::uint32_t>(s.particles.size()) : 0;
    spawn = std::min(spawn, room);
    const float life_lo = std::max(0.001f, std::min(e.lifetime_min, e.lifetime_max));
    const float life_hi = std::max(life_lo, std::max(e.lifetime_min, e.lifetime_max));
    const float spin_to_rad = kPi / 180.0f;
    for (std::uint32_t i = 0; i < spawn; ++i) {
        Vec3 pos, dir;
        sample_shape(e, s.rng, pos, dir);
        const float speed = s.rng.range_float(std::min(e.speed_min, e.speed_max), std::max(e.speed_min, e.speed_max));
        Particle p;
        p.life = s.rng.range_float(life_lo, life_hi);
        p.rotation = s.rng.next_float() * 2.0f * kPi;
        p.spin = s.rng.range_float(e.spin_min, e.spin_max) * spin_to_rad;
        p.size_scale = 1.0f - clamp01(e.size_variation) * s.rng.next_float();
        if (e.world_space) {
            p.position = world.transform_point(pos);
            p.velocity = math::normalize(world.transform_vector(dir)) * speed;
        } else {
            p.position = pos;
            p.velocity = dir * speed;
        }
        s.particles.push_back(p);
    }
    counts.spawned = spawn;
    s.emitted += spawn;

    // Billboards in world space.
    s.billboards.resize(s.particles.size());
    s.bounds = {};
    const float intensity = std::max(0.0f, e.intensity);
    for (std::size_t i = 0; i < s.particles.size(); ++i) {
        const Particle& p = s.particles[i];
        const float t = clamp01(p.age / p.life);
        Billboard& b = s.billboards[i];
        b.position = e.world_space ? p.position : world.transform_point(p.position);
        b.size = std::max(0.0f, math::lerp(e.size_start, e.size_end, t) * p.size_scale);
        b.color = Color(math::lerp(e.color_start.r, e.color_end.r, t) * intensity, math::lerp(e.color_start.g, e.color_end.g, t) * intensity,
                        math::lerp(e.color_start.b, e.color_end.b, t) * intensity, clamp01(math::lerp(e.color_start.a, e.color_end.a, t)));
        b.rotation = p.rotation;
        const Vec3 half{b.size * 0.5f};
        s.bounds.expand(b.position - half);
        s.bounds.expand(b.position + half);
    }
    return counts;
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
            .field(&ParticleEmitterComponent::max_particles, "max_particles", {.min = 1, .max = 65536})
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
            w.counts = simulate(*w.emitter, *w.state, w.world, dt, w.seed);
        }
    };
    if (settings_.jobs && work_.size() >= std::max<std::uint32_t>(2, settings_.parallel_threshold)) settings_.jobs->parallel_for(0, work_.size(), 1, run);
    else run(0, work_.size());

    stats_ = {};
    stats_.emitters = static_cast<std::uint32_t>(work_.size());
    for (const Work& w : work_) {
        stats_.alive += static_cast<std::uint32_t>(w.state->particles.size());
        stats_.spawned += w.counts.spawned;
        stats_.died += w.counts.died;
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
        s->bounds = {};
        s->started = false;
        s->finished = false;
        s->time = 0;
        s->accumulator = 0;
        s->pending = 0;
    }
    return true;
}

EmitterStatus status(Scene& scene, Entity e) {
    EmitterStatus out;
    ecs::World& world = scene.world();
    if (!world.has<ParticleEmitterComponent>(e)) return out;
    out.valid = true;
    if (const auto* s = world.get<EmitterState>(e)) {
        out.alive = static_cast<std::uint32_t>(s->particles.size());
        out.emitted = s->emitted;
        out.time = s->time;
        out.finished = s->finished;
    }
    return out;
}

void gather(Scene& scene, std::vector<EmitterView>& out) {
    scene.world().each<ParticleEmitterComponent, EmitterState>([&](Entity e, ParticleEmitterComponent& c, EmitterState& s) {
        if (s.billboards.empty()) return;
        out.push_back({e, &c, std::span<const Billboard>(s.billboards), s.bounds});
    });
}

} // namespace victus::particles
