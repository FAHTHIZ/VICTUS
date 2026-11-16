// SPDX-License-Identifier: Apache-2.0
// Built-in project templates. Each produces a scene that runs immediately (Play) with the
// built-in behaviours, so a new project shows motion, lighting and a camera on first launch.
#include "victus/animation/animation.hpp"
#include "victus/particles/particles.hpp"
#include "victus/audio/audio.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/physics/physics.hpp"
#include "victus/project/project.hpp"
#include "victus/script/script.hpp"

#include "robot.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace victus::project {

using namespace scene;
using gameplay::OscillatorComponent;
using gameplay::RotatorComponent;
using script::ScriptComponent;

namespace {

math::Quat look_rotation(math::Vec3 dir) {
    dir = math::normalize(dir);
    const float yaw = std::atan2(-dir.x, -dir.z);
    const float pitch = std::asin(std::clamp(dir.y, -1.0f, 1.0f));
    return math::Quat::euler(pitch, yaw, 0.0f);
}

// ---- procedural template sounds ------------------------------------------------------------
// Templates ship small synthesized WAV clips so new projects have audio without bundled media.

constexpr std::uint32_t kRate = 22050;
constexpr float kTau = 6.28318530718f;

std::string wav_string(const std::vector<float>& pcm) {
    const auto bytes = audio::encode_wav(pcm.data(), pcm.size(), 1, kRate);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::vector<float> samples(float seconds) { return std::vector<float>(static_cast<std::size_t>(seconds * kRate)); }

// Upward pitch sweep with a soft square-ish tone.
std::string synth_jump() {
    auto pcm = samples(0.16f);
    float phase = 0;
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(pcm.size());
        phase += kTau * (320.0f + 520.0f * t) / kRate;
        const float tone = std::tanh(2.5f * std::sin(phase));
        pcm[i] = 0.32f * tone * (1.0f - t) * std::min(1.0f, t * 40.0f);
    }
    return wav_string(pcm);
}

// Two-note bell blip (B5 then E6).
std::string synth_coin() {
    auto pcm = samples(0.32f);
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        const float s = static_cast<float>(i) / kRate;
        const float f = s < 0.07f ? 987.77f : 1318.51f;
        const float local = s < 0.07f ? s : s - 0.07f;
        const float env = std::exp(-local * 9.0f) * std::min(1.0f, local * 400.0f);
        pcm[i] = 0.3f * env * (std::sin(kTau * f * s) + 0.3f * std::sin(kTau * 2.0f * f * s));
    }
    return wav_string(pcm);
}

// Low thud: decaying sine drop plus a short noise burst (deterministic LCG noise).
std::string synth_impact() {
    auto pcm = samples(0.22f);
    std::uint32_t seed = 12345u;
    float phase = 0;
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        const float s = static_cast<float>(i) / kRate;
        phase += kTau * (140.0f - 260.0f * s) / kRate;
        seed = seed * 1664525u + 1013904223u;
        const float noise = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
        pcm[i] = 0.6f * std::exp(-s * 22.0f) * std::sin(phase) + 0.18f * std::exp(-s * 60.0f) * noise;
    }
    return wav_string(pcm);
}

// Four-bar arpeggio (Am - F - C - G) with soft plucks; loops seamlessly at 4 s.
std::string synth_music() {
    auto pcm = samples(4.0f);
    const float chords[4][3] = {{220.00f, 261.63f, 329.63f},
                                {174.61f, 220.00f, 261.63f},
                                {261.63f, 329.63f, 392.00f},
                                {196.00f, 246.94f, 293.66f}};
    constexpr float kStep = 0.125f; // eighth notes at 120 BPM
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        const float s = static_cast<float>(i) / kRate;
        const int bar = std::min(3, static_cast<int>(s));
        const int step = static_cast<int>(s / kStep);
        const float local = s - static_cast<float>(step) * kStep;
        const float f = chords[bar][step % 3] * ((step % 6) >= 3 ? 2.0f : 1.0f);
        const float pluck = std::exp(-local * 14.0f) * std::min(1.0f, local * 300.0f);
        const float bass = 0.5f * std::sin(kTau * chords[bar][0] * 0.5f * s);
        const float bar_local = s - static_cast<float>(bar);
        const float bass_env = std::min(1.0f, bar_local * 50.0f) * std::min(1.0f, (1.0f - bar_local) * 50.0f);
        pcm[i] = 0.16f * pluck * (std::sin(kTau * f * s) + 0.25f * std::sin(kTau * 3.0f * f * s)) + 0.1f * bass * bass_env;
    }
    return wav_string(pcm);
}

void attach(Scene& s, Entity e, std::string_view path, json::Value properties = json::Value::object()) {
    auto& c = s.world().add<ScriptComponent>(e);
    c.script = std::string(path);
    c.properties = std::move(properties);
}

Entity add(Scene& s, std::string_view name, math::Vec3 pos, Entity parent = kNullEntity) {
    const Entity e = s.create(name, parent);
    s.world().get<Transform>(e)->position = pos;
    return e;
}

particles::ParticleEmitterComponent& emitter(Scene& s, Entity e) { return s.world().add<particles::ParticleEmitterComponent>(e); }

MeshRendererComponent& mesh(Scene& s, Entity e, std::string_view primitive, math::Color color, float metallic,
                            float roughness) {
    auto& m = s.world().add<MeshRendererComponent>(e);
    m.primitive = std::string(primitive);
    m.base_color = color;
    m.metallic = metallic;
    m.roughness = roughness;
    return m;
}

Entity camera_3d(Scene& s, math::Vec3 eye, math::Vec3 target) {
    const Entity cam = add(s, "Main Camera", eye);
    s.world().get<Transform>(cam)->rotation = look_rotation(target - eye);
    s.world().add<CameraComponent>(cam);
    return cam;
}

void sun(Scene& s) {
    const Entity e = add(s, "Sun", {0, 10, 0});
    s.world().get<Transform>(e)->rotation = look_rotation({-0.4f, -1.0f, -0.3f});
    auto& l = s.world().add<LightComponent>(e);
    l.type = LightComponent::Type::Directional;
    l.color = {1.0f, 0.96f, 0.9f, 1};
    l.intensity = 3.0f;
}

void build_3d(Scene& s) {
    attach(s, camera_3d(s, {0, 4.5f, 9}, {0, 0.6f, 0}), "scripts/follow_camera.luau");
    sun(s);
    {
        // Scene look: filmic tonemapping with subtle bloom and contact shadows (SSAO).
        auto& env = s.world().add<gameplay::EnvironmentComponent>(add(s, "Environment", {0, 0, 0}));
        env.bloom_intensity = 0.04f;
        env.ao_intensity = 1.0f;
        env.shadow_distance = 30.0f;
    }
    const Entity ground = add(s, "Ground", {0, 0, 0});
    s.world().get<Transform>(ground)->scale = {20, 1, 20};
    mesh(s, ground, "plane", {0.16f, 0.17f, 0.19f, 1}, 0.0f, 0.9f);
    {
        s.world().add<physics::RigidBodyComponent>(ground).type = physics::BodyType::Static;
        auto& c = s.world().add<physics::ColliderComponent>(ground);
        c.size = {1, 0.2f, 1}; // scaled by the transform to 20 x 0.2 x 20, top face at y=0
        c.offset = {0, -0.1f, 0};
    }

    const Entity cube = add(s, "Spinning Cube", {-2.2f, 0.75f, 0});
    s.world().get<Transform>(cube)->scale = {1.5f, 1.5f, 1.5f};
    mesh(s, cube, "cube", {0.9f, 0.32f, 0.08f, 1}, 0.0f, 0.45f);
    s.world().add<RotatorComponent>(cube) = {{0, 1, 0}, 40.0f};
    s.world().add<physics::RigidBodyComponent>(cube).type = physics::BodyType::Kinematic; // turned by Rotator
    s.world().add<physics::ColliderComponent>(cube);

    // The player is a CharacterController (origin at the feet) with the animated robot as its visual:
    // it walks, climbs the steps, jumps and shoves the dynamic crates and the gate.
    const Entity player = add(s, "Player", {0, 0, 0.5f});
    attach(s, player, "scripts/player.luau");
    {
        auto& cc = s.world().add<physics::CharacterControllerComponent>(player);
        cc.radius = 0.7f;
        cc.height = 1.4f;
        cc.push_force = 400.0f;
    }
    // The visible character is the skinned template robot (assets/models/robot.gltf), scaled to
    // the 1.4 m capsule; the player script plays its "Idle"/"Walk" clips and turns it to face
    // the direction of travel.
    const Entity robot = robot::instantiate(s, "Robot", player);
    s.world().get<Transform>(robot)->scale = {0.85f, 0.85f, 0.85f};
    {
        // Dust kicked up on jumps and landings: bursts emitted by player.luau.
        auto& d = emitter(s, add(s, "Dust", {0, 0.05f, 0}, player));
        d.emitting = false;
        d.max_particles = 64;
        d.shape = particles::Shape::Cone;
        d.radius = 0.35f;
        d.angle = 80;
        d.speed_min = 0.8f;
        d.speed_max = 1.8f;
        d.drag = 3.0f;
        d.gravity = {0, 0.4f, 0};
        d.lifetime_min = 0.45f;
        d.lifetime_max = 0.8f;
        d.size_start = 0.25f;
        d.size_end = 0.7f;
        d.size_variation = 0.4f;
        d.color_start = {0.62f, 0.6f, 0.56f, 0.45f};
        d.color_end = {0.62f, 0.6f, 0.56f, 0.0f};
        d.spin_min = -60;
        d.spin_max = 60;
    }

    // Three 0.3 m steps up to a platform (character step_height is 0.35 m).
    for (int i = 0; i < 3; ++i) {
        const float h = 0.3f * static_cast<float>(i + 1);
        const Entity step = add(s, "Step " + std::to_string(i + 1), {-5.0f - 0.8f * static_cast<float>(i), h * 0.5f, -4.0f});
        s.world().get<Transform>(step)->scale = {0.8f, h, 3.0f};
        mesh(s, step, "cube", {0.55f, 0.57f, 0.6f, 1}, 0.0f, 0.8f);
        s.world().add<physics::ColliderComponent>(step);
    }

    // A swinging gate on a hinge joint (limited to +-100 degrees) that the player pushes open.
    {
        const Entity post = add(s, "Gate Post", {-6.0f, 1.0f, 4.0f});
        s.world().get<Transform>(post)->scale = {0.2f, 2.0f, 0.2f};
        mesh(s, post, "cube", {0.3f, 0.3f, 0.32f, 1}, 1.0f, 0.4f);
        s.world().add<physics::ColliderComponent>(post);
        const Entity gate = add(s, "Gate", {-5.0f, 1.0f, 4.0f});
        s.world().get<Transform>(gate)->scale = {1.8f, 1.6f, 0.1f};
        mesh(s, gate, "cube", {0.75f, 0.2f, 0.18f, 1}, 0.0f, 0.6f);
        s.world().add<physics::ColliderComponent>(gate);
        auto& rb = s.world().add<physics::RigidBodyComponent>(gate);
        rb.mass = 6.0f;
        rb.angular_damping = 0.8f;
        auto& j = s.world().add<physics::JointComponent>(gate);
        j.type = physics::JointComponent::Type::Hinge;
        j.connected = s.uuid(post);
        j.anchor = {-0.5f, 0, 0}; // local: the gate's left edge (scaled by the transform)
        j.axis = {0, 1, 0};
        j.limits = true;
        j.min_limit = -100;
        j.max_limit = 100;
    }

    for (int i = 0; i < 3; ++i) {
        const Entity crate = add(s, "Crate " + std::to_string(i + 1),
                                 {3.6f + 0.05f * static_cast<float>(i), 0.3f + 0.62f * static_cast<float>(i), 2.0f});
        s.world().get<Transform>(crate)->scale = {0.6f, 0.6f, 0.6f};
        mesh(s, crate, "cube", {0.62f, 0.45f, 0.28f, 1}, 0.0f, 0.7f);
        s.world().add<physics::RigidBodyComponent>(crate).mass = 4.0f;
        s.world().add<physics::ColliderComponent>(crate).friction = 0.7f;
        attach(s, crate, "scripts/crate.luau");
    }

    const Entity bob = add(s, "Floating Gem", {1.0f, 2.6f, -2.0f});
    s.world().get<Transform>(bob)->scale = {0.5f, 0.5f, 0.5f};
    mesh(s, bob, "cube", {0.2f, 0.85f, 0.45f, 1}, 0.0f, 0.3f);
    s.world().add<OscillatorComponent>(bob) = {{0, 0.4f, 0}, 0.4f, 0.0f, {}, false};
    s.world().add<RotatorComponent>(bob) = {{1, 1, 0}, 60.0f};

    const Entity pillar = add(s, "Metal Pillar", {2.4f, 1.0f, -0.4f});
    s.world().get<Transform>(pillar)->scale = {1.0f, 2.0f, 1.0f};
    mesh(s, pillar, "cylinder", {0.95f, 0.93f, 0.88f, 1}, 1.0f, 0.3f);
    s.world().add<physics::RigidBodyComponent>(pillar).type = physics::BodyType::Static;
    s.world().add<physics::ColliderComponent>(pillar).shape = physics::ColliderComponent::Shape::Cylinder;

    // A brazier whose fire, sparks and smoke are particle emitters; the warm lamp above it is the
    // fire's light. Fire and sparks are additive and HDR (> 1) so they glow through bloom.
    const Entity brazier = add(s, "Brazier", {0.5f, 0.3f, 2.5f});
    s.world().get<Transform>(brazier)->scale = {0.7f, 0.6f, 0.7f};
    mesh(s, brazier, "cylinder", {0.18f, 0.17f, 0.16f, 1}, 1.0f, 0.55f);
    s.world().add<physics::RigidBodyComponent>(brazier).type = physics::BodyType::Static;
    s.world().add<physics::ColliderComponent>(brazier).shape = physics::ColliderComponent::Shape::Cylinder;
    {
        auto& f = emitter(s, add(s, "Fire", {0.5f, 0.62f, 2.5f}));
        f.blend = particles::Blend::Additive;
        f.rate = 70;
        f.max_particles = 128;
        f.shape = particles::Shape::Cone;
        f.radius = 0.2f;
        f.angle = 12;
        f.speed_min = 0.5f;
        f.speed_max = 1.1f;
        f.gravity = {0, 1.2f, 0}; // buoyancy
        f.drag = 1.2f;
        f.lifetime_min = 0.45f;
        f.lifetime_max = 0.85f;
        f.size_start = 0.5f;
        f.size_end = 0.08f;
        f.size_variation = 0.3f;
        f.color_start = {1.0f, 0.55f, 0.16f, 1};
        f.color_end = {0.8f, 0.08f, 0.02f, 0};
        f.intensity = 2.5f;
        f.spin_min = -90;
        f.spin_max = 90;
        f.softness = 0.15f;
    }
    {
        auto& sp = emitter(s, add(s, "Sparks", {0.5f, 0.7f, 2.5f}));
        sp.blend = particles::Blend::Additive;
        sp.rate = 14;
        sp.max_particles = 64;
        sp.shape = particles::Shape::Point;
        sp.angle = 25;
        sp.speed_min = 1.6f;
        sp.speed_max = 3.2f;
        sp.gravity = {0, -1.8f, 0};
        sp.drag = 0.6f;
        sp.lifetime_min = 0.8f;
        sp.lifetime_max = 1.6f;
        sp.size_start = 0.06f;
        sp.size_end = 0.02f;
        sp.color_start = {1.0f, 0.82f, 0.45f, 1};
        sp.color_end = {1.0f, 0.3f, 0.05f, 0};
        sp.intensity = 6.0f;
    }
    {
        auto& sm = emitter(s, add(s, "Smoke", {0.5f, 1.15f, 2.5f}));
        sm.rate = 6;
        sm.max_particles = 48;
        sm.shape = particles::Shape::Cone;
        sm.radius = 0.15f;
        sm.angle = 10;
        sm.speed_min = 0.35f;
        sm.speed_max = 0.6f;
        sm.gravity = {0.15f, 0.1f, 0}; // a slight breeze
        sm.lifetime_min = 2.2f;
        sm.lifetime_max = 3.2f;
        sm.size_start = 0.35f;
        sm.size_end = 1.5f;
        sm.color_start = {0.22f, 0.21f, 0.2f, 0.32f};
        sm.color_end = {0.3f, 0.3f, 0.3f, 0};
        sm.spin_min = -20;
        sm.spin_max = 20;
        sm.softness = 0.5f;
    }

    const Entity lamp = add(s, "Warm Lamp", {0.5f, 1.4f, 2.5f});
    auto& l = s.world().add<LightComponent>(lamp);
    l.type = LightComponent::Type::Point;
    l.color = {1.0f, 0.62f, 0.3f, 1};
    l.intensity = 12.0f;
    l.range = 7.0f;
}

SpriteComponent& sprite(Scene& s, Entity e, math::Vec2 size, math::Color tint, std::int32_t layer) {
    auto& sp = s.world().add<SpriteComponent>(e);
    sp.size = size;
    sp.tint = tint;
    sp.layer = layer;
    return sp;
}

Entity solid_2d(Scene& s, std::string_view name, math::Vec3 at, math::Vec2 size, math::Color color, float friction) {
    const Entity e = add(s, name, at);
    sprite(s, e, size, color, 0);
    auto& rb = s.world().add<physics::RigidBody2DComponent>(e);
    rb.type = physics::BodyType::Static;
    auto& c = s.world().add<physics::Collider2DComponent>(e);
    c.size = size;
    c.friction = friction;
    return e;
}

void build_2d(Scene& s) {
    const Entity cam = add(s, "Main Camera", {0, 0, 10});
    auto& c = s.world().add<CameraComponent>(cam);
    c.projection = CameraComponent::Projection::Orthographic;
    c.ortho_height = 10.0f;

    sprite(s, add(s, "Sky", {0, 1, -1}), {40, 14}, {0.38f, 0.6f, 0.9f, 1}, -10);
    sprite(s, add(s, "Sun Disc", {5.5f, 3.2f, -0.5f}), {1.6f, 1.6f}, {1.0f, 0.86f, 0.35f, 1}, -9);
    const math::Color dirt{0.45f, 0.3f, 0.2f, 1}, wall{0.3f, 0.22f, 0.16f, 1};
    solid_2d(s, "Ground", {0, -4.2f, 0}, {40, 2.4f}, {0.22f, 0.55f, 0.27f, 1}, 0.6f);
    // One-way: jump up through it from below, land on top, press S/Down to drop back through.
    const Entity ledge = solid_2d(s, "Platform", {2.5f, -1.0f, 0}, {3.0f, 0.4f}, {0.62f, 0.44f, 0.26f, 1}, 0.6f);
    s.world().get<physics::Collider2DComponent>(ledge)->one_way = true;
    solid_2d(s, "Platform High", {-1.5f, 1.0f, 0}, {2.5f, 0.4f}, dirt, 0.6f);
    // Frictionless walls keep the player inside the camera view (half width ~8.9 at 16:9)
    // without letting it cling to them mid-air.
    solid_2d(s, "Platform Wall Left", {-9.3f, 1.0f, 0}, {1.0f, 14.0f}, wall, 0.0f);
    solid_2d(s, "Platform Wall Right", {9.3f, 1.0f, 0}, {1.0f, 14.0f}, wall, 0.0f);

    // A lantern swinging on a revolute joint from a wall bracket: bump it or jump into it.
    sprite(s, add(s, "Bracket", {-6.5f, 2.45f, 0}), {0.7f, 0.12f}, wall, 1);
    const Entity lantern = add(s, "Lantern", {-6.5f, 1.5f, 0});
    sprite(s, lantern, {0.36f, 0.5f}, {1.0f, 0.72f, 0.3f, 1}, 1);
    sprite(s, add(s, "Chain", {0, 0.57f, 0}, lantern), {0.05f, 0.65f}, {0.2f, 0.18f, 0.16f, 1}, 0);
    auto& lrb = s.world().add<physics::RigidBody2DComponent>(lantern);
    lrb.mass = 0.6f;
    lrb.angular_damping = 0.3f;
    s.world().add<physics::Collider2DComponent>(lantern).size = {0.36f, 0.5f};
    auto& hinge = s.world().add<physics::Joint2DComponent>(lantern);
    hinge.type = physics::Joint2DComponent::Type::Revolute;
    hinge.anchor = {0, 0.9f}; // the bracket, 0.9 m above the lantern's centre

    const Entity player = add(s, "Player", {-4.0f, -2.55f, 0});
    sprite(s, player, {0.9f, 0.9f}, {0.95f, 0.45f, 0.15f, 1}, 2);
    auto& pc = s.world().add<physics::CharacterController2DComponent>(player);
    pc.size = {0.86f, 0.9f};
    pc.gravity_scale = 2.6f; // snappier arcs than real-world gravity
    pc.air_control = 0.8f;
    pc.push_force = 30.0f;
    attach(s, player, "scripts/platformer.luau");

    const math::Vec3 coins[] = {{2.5f, -0.2f, 0}, {-1.5f, 1.8f, 0}, {6.0f, -2.4f, 0}};
    int n = 0;
    for (const math::Vec3& at : coins) {
        const Entity coin = add(s, "Coin " + std::to_string(++n), at);
        sprite(s, coin, {0.5f, 0.5f}, {1.0f, 0.78f, 0.12f, 1}, 1);
        auto& rb = s.world().add<physics::RigidBody2DComponent>(coin);
        rb.type = physics::BodyType::Kinematic; // moved by coin.luau
        auto& cc = s.world().add<physics::Collider2DComponent>(coin);
        cc.shape = physics::Collider2DComponent::Shape::Circle;
        cc.radius = 0.3f;
        cc.is_trigger = true;
        attach(s, coin, "scripts/coin.luau");
        // A faint glitter around each coin (a child, so it disappears with the coin).
        auto& g = emitter(s, add(s, "Glitter", {}, coin));
        g.blend = particles::Blend::Additive;
        g.rate = 7;
        g.max_particles = 16;
        g.shape = particles::Shape::Sphere;
        g.radius = 0.35f;
        g.speed_min = 0.05f;
        g.speed_max = 0.25f;
        g.lifetime_min = 0.5f;
        g.lifetime_max = 0.9f;
        g.size_start = 0.2f;
        g.size_end = 0.0f;
        g.color_start = {1.0f, 0.95f, 0.6f, 1};
        g.color_end = {1.0f, 0.8f, 0.3f, 0};
        g.layer = 3;
    }
    {
        // Burst at a collected coin: platformer.luau moves this emitter there and emits.
        auto& sp = emitter(s, add(s, "Sparkles", {0, 0, 0}));
        sp.emitting = false;
        sp.blend = particles::Blend::Additive;
        sp.max_particles = 128;
        sp.shape = particles::Shape::Sphere;
        sp.radius = 0.1f;
        sp.speed_min = 2.0f;
        sp.speed_max = 4.0f;
        sp.drag = 2.5f;
        sp.gravity = {0, -4.0f, 0};
        sp.lifetime_min = 0.4f;
        sp.lifetime_max = 0.8f;
        sp.size_start = 0.2f;
        sp.size_end = 0.02f;
        sp.color_start = {1.0f, 0.88f, 0.35f, 1};
        sp.color_end = {1.0f, 0.45f, 0.1f, 0};
        sp.layer = 5;
    }

    const Entity music = add(s, "Music", {0, 0, 0});
    auto& ms = s.world().add<audio::AudioSourceComponent>(music);
    ms.clip = "audio/music.wav";
    ms.bus = audio::Bus::Music;
    ms.volume = 0.35f;
    ms.loop = true;
    ms.stream = true;
    ms.spatial = false;

    const Entity spinner = add(s, "Spinner", {4.5f, 0.2f, 0});
    sprite(s, spinner, {1.2f, 1.2f}, {0.95f, 0.85f, 0.2f, 1}, 1);
    s.world().add<RotatorComponent>(spinner) = {{0, 0, 1}, 90.0f};
}

void build_simulation(Scene& s) {
    attach(s, camera_3d(s, {0, 14, 18}, {0, 0, 0}), "scripts/orbit_camera.luau");
    sun(s);
    const Entity ground = add(s, "Ground", {0, -0.6f, 0});
    s.world().get<Transform>(ground)->scale = {40, 1, 40};
    mesh(s, ground, "plane", {0.3f, 0.32f, 0.35f, 1}, 0.0f, 0.95f);
    const Entity field = add(s, "Wave Field", {0, 0, 0});
    constexpr int kSide = 24;
    for (int z = 0; z < kSide; ++z) {
        for (int x = 0; x < kSide; ++x) {
            const float px = (static_cast<float>(x) - kSide * 0.5f) * 0.8f;
            const float pz = (static_cast<float>(z) - kSide * 0.5f) * 0.8f;
            const Entity cell = add(s, "Cell", {px, 0, pz}, field);
            s.world().get<Transform>(cell)->scale = {0.6f, 0.6f, 0.6f};
            const float d = std::sqrt(px * px + pz * pz);
            const float hue = 0.5f + 0.5f * std::sin(d * 0.4f);
            mesh(s, cell, "cube", {0.1f + 0.8f * hue, 0.35f, 1.0f - 0.8f * hue, 1}, 0.0f, 0.5f);
            s.world().add<OscillatorComponent>(cell) = {{0, 0.6f, 0}, 0.35f, -d * 0.6f, {}, false};
        }
    }
}

// ---- template scripts ------------------------------------------------------------------------

constexpr const char* kPlayer3d = R"lua(-- Walks the character with WASD or the arrow keys and jumps with Space. The entity has a
-- CharacterController (it slides along walls, climbs steps and pushes physics bodies); the
-- child "Robot" model faces the direction of travel and cross-fades between its "Idle" and
-- "Walk" animation clips. Its child "Dust" particle emitter puffs on jumps and landings.
-- Fields below are defaults; override them per entity in the editor inspector.
local Player = {}
Player.speed = 5
Player.jump_speed = 6
Player.bounds = 9 -- stay within this distance of the origin on X and Z

function Player:start()
    self.robot = self.entity:find_child("Robot")
    self.dust = self.entity:find_child("Dust")
    self.clip = "Idle"
end

function Player:update(dt)
    local x = input.axis("A", "D", "Left", "Right")
    local z = input.axis("W", "S", "Up", "Down")
    local move = vector.create(x, 0, z)
    if vector.magnitude(move) > 0 then move = vector.normalize(move) * self.speed end
    -- Stop at the play-area bounds (only the outward component).
    local p = self.entity.position
    if (p.x >= self.bounds and move.x > 0) or (p.x <= -self.bounds and move.x < 0) then
        move = vector.create(0, 0, move.z)
    end
    if (p.z >= self.bounds and move.z > 0) or (p.z <= -self.bounds and move.z < 0) then
        move = vector.create(move.x, 0, 0)
    end
    self.entity:move_character(move)
    local grounded = self.entity.grounded
    if input.pressed("Space") and grounded then
        self.entity:jump(self.jump_speed)
        if self.dust then self.dust:emit_particles(14) end
    end
    -- Landing puff: grounded again after being airborne.
    if grounded and self.was_grounded == false and self.dust then self.dust:emit_particles(10) end
    self.was_grounded = grounded
    if not self.robot then return end
    -- Face the direction actually travelled (the robot model looks down +Z) and pick the clip.
    local v = self.entity.velocity
    local speed = vector.magnitude(vector.create(v.x, 0, v.z))
    if speed > 0.3 then
        self.robot.rotation = vector.create(0, math.deg(math.atan2(v.x, v.z)), 0)
    end
    local clip = (speed > 0.3 and self.entity.grounded) and "Walk" or "Idle"
    if clip ~= self.clip then
        self.clip = clip
        self.robot:play_animation(clip, 0.2)
    end
end

return Player
)lua";

constexpr const char* kFollowCamera = R"lua(-- Smoothly follows the entity named `target`, keeping the starting offset.
local Follow = {}
Follow.target = "Player"
Follow.smoothing = 4 -- higher follows more tightly

function Follow:start()
    self.subject = scene.find(self.target)
    if self.subject then
        self.offset = self.entity.position - self.subject.position
    end
end

function Follow:update(dt)
    if not self.subject or not self.subject.valid then return end
    local goal = self.subject.position + self.offset
    local k = 1 - math.exp(-self.smoothing * dt)
    self.entity.position = self.entity.position + (goal - self.entity.position) * k
    self.entity:look_at(self.subject.position)
end

return Follow
)lua";

constexpr const char* kPlatformer = R"lua(-- Platformer controller on a CharacterController2D: A/D or Left/Right to run, Space, W or Up
-- to jump, S or Down to drop through one-way platforms. The Box2D mover slides along walls,
-- lands on one-way platforms only from above and nudges physics bodies (the lantern); coins
-- are trigger colliders collected in on_trigger_enter.
local Player = {}
Player.speed = 6
Player.jump_speed = 12
Player.jump_sound = "audio/jump.wav"
Player.coin_sound = "audio/coin.wav"

local function starts_with(s, prefix)
    return string.sub(s, 1, #prefix) == prefix
end

function Player:start()
    self.spawn = self.entity.position
    self.score = 0
    self.total = 0
    for _, e in scene.query("Collider2D") do
        if starts_with(e.name, "Coin") then self.total += 1 end
    end
end

function Player:fixed_update(dt)
    local e = self.entity
    e:move_character(vector.create(input.axis("A", "D", "Left", "Right") * self.speed, 0, 0))
    if (input.down("S") or input.down("Down")) and e.grounded then
        e:drop_through()
    elseif (input.down("Space") or input.down("W") or input.down("Up")) and e:jump(self.jump_speed) then
        audio.play(self.jump_sound, { volume = 0.6 })
    end
    if e.position.y < -20 then -- fell off the world
        e.position = self.spawn
        e.velocity = vector.create(0, 0, 0)
    end
end

function Player:on_trigger_enter(other)
    if starts_with(other.name, "Coin") then
        local fx = scene.find("Sparkles")
        if fx then
            fx.position = other.position
            fx:emit_particles(28)
        end
        other:destroy()
        audio.play(self.coin_sound)
        self.score += 1
        log.info("Coin collected:", self.score, "/", self.total)
        if self.score == self.total then log.info("All coins collected!") end
    end
end

return Player
)lua";

constexpr const char* kCrate = R"lua(-- Plays a positional impact sound when the crate hits something hard enough.
local Crate = {}
Crate.sound = "audio/impact.wav"
Crate.min_speed = 1.5 -- m/s before impact
Crate.cooldown = 0.12

function Crate:start()
    self.speed = 0
    self.last = -1
end

function Crate:update(dt)
    -- Runs before the physics step, so this is the speed going into any contact this frame.
    self.speed = vector.magnitude(self.entity.velocity)
end

function Crate:on_collision_enter(other, info)
    local now = time.now()
    if self.speed >= self.min_speed and now - self.last >= self.cooldown then
        self.last = now
        audio.play(self.sound, { position = info.point, volume = math.min(1, self.speed / 6),
                                 pitch = 0.9 + math.random() * 0.2 })
    end
end

return Crate
)lua";

constexpr const char* kCoin = R"lua(-- Spins and hovers (a kinematic trigger body); the player script collects it on contact.
local Coin = {}
Coin.spin_speed = 180
Coin.hover = 0.12

function Coin:start()
    self.base = self.entity.position
    self.t = math.random() * 6
end

function Coin:update(dt)
    self.t += dt
    self.entity:rotate(vector.create(0, 0, 1), self.spin_speed * dt)
    self.entity.position = self.base + vector.create(0, math.sin(self.t * 3) * self.hover, 0)
end

return Coin
)lua";

constexpr const char* kOrbitCamera = R"lua(-- Orbits around `center` at the starting distance and height, looking at the center.
local Orbit = {}
Orbit.degrees_per_second = 8
Orbit.center = vector.create(0, 0, 0)

function Orbit:start()
    local d = self.entity.position - self.center
    self.radius = math.sqrt(d.x * d.x + d.z * d.z)
    self.height = d.y
    self.angle = math.atan2(d.x, d.z)
end

function Orbit:update(dt)
    self.angle += math.rad(self.degrees_per_second) * dt
    self.entity.position = self.center
        + vector.create(math.sin(self.angle) * self.radius, self.height, math.cos(self.angle) * self.radius)
    self.entity:look_at(self.center)
end

return Orbit
)lua";

} // namespace

const std::vector<TemplateInfo>& templates() {
    static const std::vector<TemplateInfo> list = {
        {"3d", "3D Scene", "An animated robot character you walk with WASD/arrow keys and jump with Space (Luau + CharacterController) that climbs steps, swings a hinged gate and shoves a stack of physics crates with positional impact sounds, a follow camera, sun shadows, PBR primitives and a point light."},
        {"2d", "2D Game", "A playable platformer on Box2D physics with music and sound effects: a CharacterController2D you run with A/D or arrows and jump with Space, a one-way ledge you jump through and drop off with S/Down, a lantern swinging on a joint and coins to collect. Gameplay is in Luau scripts."},
        {"simulation", "Simulation",
         "576 entities animated by native systems, with a scripted orbiting camera: a starting point for simulation work."},
    };
    return list;
}

std::vector<TemplateFile> template_files(std::string_view id) {
    if (id == "3d")
        return {{"assets/scripts/player.luau", kPlayer3d},
                {robot::kModelPath, robot::gltf()},
                {std::string(robot::kModelPath) + ".meta", robot::meta()},
                {"assets/scripts/follow_camera.luau", kFollowCamera},
                {"assets/scripts/crate.luau", kCrate},
                {"assets/audio/impact.wav", synth_impact()}};
    if (id == "2d")
        return {{"assets/scripts/platformer.luau", kPlatformer},
                {"assets/scripts/coin.luau", kCoin},
                {"assets/audio/jump.wav", synth_jump()},
                {"assets/audio/coin.wav", synth_coin()},
                {"assets/audio/music.wav", synth_music()}};
    if (id == "simulation") return {{"assets/scripts/orbit_camera.luau", kOrbitCamera}};
    return {};
}

void register_components() {
    gameplay::register_components();
    script::register_components();
    physics::register_components();
    audio::register_components();
    animation::register_components();
    particles::register_components();
}

Status build_template_scene(std::string_view id, Scene& s) {
    s.clear();
    if (id == "3d") build_3d(s);
    else if (id == "2d") build_2d(s);
    else if (id == "simulation") build_simulation(s);
    else return make_error(ErrorCode::InvalidArgument, "unknown template '" + std::string(id) + "'");
    s.update_transforms();
    return {};
}

} // namespace victus::project
