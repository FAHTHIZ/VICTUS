// SPDX-License-Identifier: Apache-2.0
// Built-in project templates. Each produces a scene that runs immediately (Play) with the
// built-in behaviours, so a new project shows motion, lighting and a camera on first launch.
#include "victus/animation/animation.hpp"
#include "victus/assets/material.hpp"
#include "victus/render/image.hpp"
#include "victus/ai/ai.hpp"
#include "victus/sequencer/sequencer.hpp"
#include "victus/navigation/navigation.hpp"
#include "victus/terrain/terrain.hpp"
#include "victus/tilemap/tilemap.hpp"
#include "victus/world/world.hpp"
#include "victus/net/net.hpp"
#include "victus/particles/particles.hpp"
#include "victus/gameui/gameui.hpp"
#include "victus/audio/audio.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/physics/physics.hpp"
#include "victus/project/characters.hpp"
#include "victus/project/project.hpp"
#include "victus/core/log.hpp"
#include "victus/scene/prefab.hpp"
#include "victus/script/behaviours.hpp"
#include "victus/script/script.hpp"
#include "sprite_art.hpp"

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

// The 3D template's crate prefab (assets/prefabs/crate.vprefab): a physics box with an impact
// sound script. The scene's crates are instances of it and the player drops more at runtime.
constexpr const char* kCratePrefabPath = "assets/prefabs/crate.vprefab";
constexpr const char* kCratePrefabUuid = "5c0a7e11-7f1b-4c2e-9a3d-c4a7e5b0f001";
constexpr const char* kCrateLocalUuid = "5c0a7e11-7f1b-4c2e-9a3d-c4a7e5b0f002";

Prefab crate_prefab() {
    Scene s;
    Uuid local;
    (void)Uuid::parse(kCrateLocalUuid, local);
    const Entity crate = s.create("Crate", kNullEntity, local);
    s.world().get<Transform>(crate)->scale = {0.6f, 0.6f, 0.6f};
    mesh(s, crate, "cube", {0.62f, 0.45f, 0.28f, 1}, 0.0f, 0.7f);
    s.world().add<physics::RigidBodyComponent>(crate).mass = 4.0f;
    s.world().add<physics::ColliderComponent>(crate).friction = 0.7f;
    attach(s, crate, "scripts/crate.luau");
    return make_prefab(s, crate);
}

// The 3D template's ground: a tiling grid texture and the material that tiles it 10x across the
// 20 m floor (2 m squares), so new projects show how materials, textures and UV tiling fit together.
constexpr const char* kGroundTexturePath = "assets/textures/ground_grid.png";
constexpr const char* kGroundTextureUuid = "5c0a7e11-7f1b-4c2e-9a3d-c4a7e5b0f002";
constexpr const char* kGroundMaterialPath = "assets/materials/ground.vmat";
constexpr const char* kGroundMaterialUuid = "5c0a7e11-7f1b-4c2e-9a3d-c4a7e5b0f003";

std::string ground_texture() {
    render::Image img;
    img.width = img.height = 64;
    img.pixels.resize(64u * 64u * 4u);
    for (std::uint32_t y = 0; y < 64; ++y)
        for (std::uint32_t x = 0; x < 64; ++x) {
            const bool line = x < 2 || y < 2;              // tile border (one per repeat)
            const bool sub = x == 32 || y == 32;           // faint half-tile line
            const std::uint8_t noise = static_cast<std::uint8_t>((x * 73u + y * 151u + x * y * 7u) % 7u);
            const std::uint8_t v = static_cast<std::uint8_t>((line ? 0x4c : sub ? 0x62 : 0x6c) + noise);
            std::uint8_t* p = img.pixels.data() + (std::size_t{y} * 64 + x) * 4;
            p[0] = v;
            p[1] = static_cast<std::uint8_t>(v + 2);
            p[2] = static_cast<std::uint8_t>(v + 6);
            p[3] = 255;
        }
    auto png = render::encode_png(img);
    return png ? std::string(png->begin(), png->end()) : std::string();
}

std::string asset_meta(const char* uuid, const char* importer) {
    json::Value m = json::Value::object();
    m.set("victus_meta", 1);
    m.set("uuid", uuid);
    m.set("importer", importer);
    return json::stringify(m, true) + "\n";
}

std::string ground_material() {
    assets::MaterialAsset m;
    (void)Uuid::parse(kGroundTextureUuid, m.base_color_texture);
    m.roughness = 0.9f;
    m.uv_scale = {10, 10};
    return json::stringify(m.to_json(), true) + "\n";
}

// The spinning cube's look: a project surface shader (glowing bands that scroll up the cube plus
// a rim glow) driven by per-material parameters, as a starting point for custom shaders.
constexpr const char* kEnergyShaderPath = "assets/shaders/energy.vshader";
constexpr const char* kEnergyShaderUuid = "5c0a7e11-7f1b-4c2e-9a3d-c4a7e5b0f004";
constexpr const char* kEnergyMaterialPath = "assets/materials/energy_cube.vmat";
constexpr const char* kEnergyMaterialUuid = "5c0a7e11-7f1b-4c2e-9a3d-c4a7e5b0f005";
constexpr const char* kEnergyShader = R"(// Energy surface shader (used by materials/energy_cube.vmat).
// Runs per pixel before lighting. See the Surface/SurfaceInput fields in the VICTUS docs:
//   i.uv, i.mesh_uv, i.world_position, i.view_dir, i.vertex_normal, i.time, i.params[0..3]
//   s.base_color, s.alpha, s.normal, s.metallic, s.roughness, s.emissive, s.occlusion, s.unlit
// Material parameters: params[0] = glow colour (rgb) + band density (w),
//                      params[1] = x glow strength, y scroll speed, z rim strength
void surface(SurfaceInput i, inout Surface s) {
    vec3 glow = i.params[0].rgb;
    float bands = 0.5 + 0.5 * sin(i.world_position.y * i.params[0].w - i.time * i.params[1].y);
    bands = smoothstep(0.7, 1.0, bands);
    float rim = pow(1.0 - clamp(dot(s.normal, i.view_dir), 0.0, 1.0), 3.0);
    s.emissive += glow * (bands * i.params[1].x + rim * i.params[1].z);
    s.roughness = mix(s.roughness, 0.2, bands); // the bands read as polished metal
    s.metallic = max(s.metallic, bands * 0.6);
}
)";

std::string energy_material() {
    assets::MaterialAsset m;
    m.base_color = {0.9f, 0.32f, 0.08f, 1};
    m.roughness = 0.45f;
    (void)Uuid::parse(kEnergyShaderUuid, m.shader);
    m.shader_params[0] = {1.0f, 0.55f, 0.15f, 6.0f};
    m.shader_params[1] = {2.5f, 2.0f, 0.3f, 0.0f};
    return json::stringify(m.to_json(), true) + "\n";
}

std::string crate_prefab_meta() {
    json::Value m = json::Value::object();
    m.set("victus_meta", 1);
    m.set("uuid", kCratePrefabUuid);
    m.set("importer", "");
    return json::stringify(m, true) + "\n";
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

// ---- In-game UI (HUD) ---------------------------------------------------------------------------

Entity ui_rect(Scene& s, Entity parent, std::string_view name, math::Vec2 anchor, math::Vec2 pivot, math::Vec2 pos,
               math::Vec2 size) {
    const Entity e = s.create(name, parent);
    gameui::UiRectComponent r;
    r.anchor_min = r.anchor_max = anchor;
    r.pivot = pivot;
    r.position = pos;
    r.size = size;
    s.world().add<gameui::UiRectComponent>(e, r);
    return e;
}

gameui::UiTextComponent& ui_text(Scene& s, Entity e, std::string_view text, float size, bool bold) {
    auto& t = s.world().add<gameui::UiTextComponent>(e);
    t.text = std::string(text);
    t.font_size = size;
    t.bold = bold;
    return t;
}

// A HUD canvas (authored at 1280x720, scaled with the window height) driven by hud.luau. Its
// "Controls" button (top-right) toggles the panel below it listing the controls (also H). With
// `saves`, a hidden "Save Status" toast (bottom centre) reports quick saves and loads.
// `properties` are hud.luau script fields (e.g. reverb_send).
Entity hud(Scene& s, std::string_view controls, math::Vec2 panel_size, bool saves = false,
           json::Value properties = json::Value::object()) {
    const Entity canvas = s.create("HUD");
    s.world().add<gameui::UiCanvasComponent>(canvas);
    attach(s, canvas, "scripts/hud.luau", std::move(properties));

    const Entity help = ui_rect(s, canvas, "Help Button", {1, 0}, {1, 0}, {-24, 24}, {164, 52});
    s.world().add<gameui::UiPanelComponent>(help).border_width = 1.5f;
    s.world().add<gameui::UiButtonComponent>(help).action = "toggle_help";
    ui_text(s, help, "Controls", 22, true).key = "hud.controls";

    const Entity panel = ui_rect(s, canvas, "Controls Panel", {1, 0}, {1, 0}, {-24, 88}, panel_size); // under the button
    auto& pp = s.world().add<gameui::UiPanelComponent>(panel);
    pp.border_width = 1.5f;
    gameui::UiRectComponent inner;
    inner.anchor_min = {0, 0};
    inner.anchor_max = {1, 1};
    inner.size = {-40, -32}; // 20 x 16 padding
    const Entity body = s.create("Controls Text", panel);
    s.world().add<gameui::UiRectComponent>(body, inner);
    auto& t = ui_text(s, body, controls, 22, false);
    t.align = gameui::TextAlign::Left;
    t.vertical_align = gameui::VerticalAlign::Top;
    t.color = {0.92f, 0.94f, 1.0f, 1};
    if (saves) {
        const Entity toast = ui_rect(s, canvas, "Save Status", {0.5f, 1}, {0.5f, 1}, {0, -32}, {420, 56});
        s.world().get<gameui::UiRectComponent>(toast)->visible = false;
        s.world().add<gameui::UiPanelComponent>(toast).border_width = 1.5f;
        ui_text(s, toast, "Saved", 24, true);
    }
    return canvas;
}

// Starter characters (victus/project/characters.hpp). Projects with a character get both
// Vanguards and the shared controller, so swapping one for the other is a reference change.
void add_character_files(std::vector<TemplateFile>& files, const TemplateOptions& options) {
    if (options.character == kNoCharacter) return;
    for (const StarterCharacter& c : starter_characters()) {
        const auto glb = starter_character_glb(c);
        files.push_back({std::string(c.model_path), std::string(reinterpret_cast<const char*>(glb.data()), glb.size())});
        files.push_back({std::string(c.model_path) + ".meta", starter_character_meta(c)});
    }
    files.push_back({kCharacterControllerPath, starter_character_controller()});
}

// The other Vanguard (the 3D template's companion is the player's counterpart).
std::string companion_character(const std::string& player) {
    if (player == kNoCharacter) return player;
    return player == "v02" ? "v01" : "v02";
}

// The visible body under a gameplay root (origin at the feet), scaled so the 1.70 m character is
// `height` tall: the chosen starter character named "Robot" (scripts find it by that name), or
// a plain capsule body for "none".
void character_visual(Scene& s, Entity parent, const std::string& character, float height) {
    if (const StarterCharacter* c = find_starter_character(character)) {
        auto robot = instantiate_starter_character(s, *c, "Robot", parent);
        if (robot) {
            const float k = height / kCharacterHeight;
            s.world().get<Transform>(*robot)->scale = {k, k, k};
            return;
        }
        VICTUS_LOG_ERROR("project", "starter character {}: {}", c->id, robot.error().message);
    }
    const Entity body = add(s, "Body", {0, height * 0.5f, 0}, parent);
    s.world().get<Transform>(body)->scale = {height * 0.36f, height * 0.5f, height * 0.36f};
    mesh(s, body, "cylinder", {0.85f, 0.84f, 0.8f, 1}, 0.0f, 0.4f);
}

void build_3d(Scene& s, const TemplateOptions& options) {
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
    (void)Uuid::parse(kGroundMaterialUuid, mesh(s, ground, "plane", {1, 1, 1, 1}, 0.0f, 0.9f).material_asset);
    {
        s.world().add<physics::RigidBodyComponent>(ground).type = physics::BodyType::Static;
        auto& c = s.world().add<physics::ColliderComponent>(ground);
        c.size = {1, 0.2f, 1}; // scaled by the transform to 20 x 0.2 x 20, top face at y=0
        c.offset = {0, -0.1f, 0};
    }

    const Entity cube = add(s, "Spinning Cube", {-2.2f, 0.75f, 0});
    s.world().get<Transform>(cube)->scale = {1.5f, 1.5f, 1.5f};
    (void)Uuid::parse(kEnergyMaterialUuid, mesh(s, cube, "cube", {1, 1, 1, 1}, 0.0f, 0.45f).material_asset);
    s.world().add<RotatorComponent>(cube) = {{0, 1, 0}, 40.0f};
    s.world().add<physics::RigidBodyComponent>(cube).type = physics::BodyType::Kinematic; // turned by Rotator
    s.world().add<physics::ColliderComponent>(cube);

    // The player is a CharacterController (origin at the feet) with the animated robot as its visual:
    // it walks, climbs the steps, jumps and shoves the dynamic crates and the gate.
    const Entity player = add(s, "Player", {0, 0, 0.5f});
    attach(s, player, "scripts/player.luau");
    s.world().add<ai::PerceptibleComponent>(player); // noticed by the sentry
    {
        auto& cc = s.world().add<physics::CharacterControllerComponent>(player);
        cc.radius = 0.7f;
        cc.height = 1.4f;
        cc.push_force = 400.0f;
    }
    // The visible character is the chosen starter character (assets/models/characters/, 1.70 m)
    // scaled to the 1.4 m capsule; the player script feeds its animation controller (speed,
    // grounded, jump, interact) and turns it to face the direction of travel.
    character_visual(s, player, options.character, 1.45f);
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

    // A small companion robot that follows the player over the navmesh (built automatically from the
    // static colliders): a NavAgent on a CharacterController, steered by scripts/companion.luau.
    {
        const Entity buddy = add(s, "Buddy", {-2.5f, 0, 3.0f});
        attach(s, buddy, "scripts/companion.luau");
        auto& cc = s.world().add<physics::CharacterControllerComponent>(buddy);
        cc.radius = 0.35f;
        cc.height = 0.8f;
        cc.push_force = 120.0f;
        auto& agent = s.world().add<navigation::NavAgentComponent>(buddy);
        agent.speed = 4.5f;
        agent.radius = 0.35f;
        agent.height = 0.8f;
        agent.stopping_distance = 0.3f;
        // The companion is the other Vanguard at half size, so the pair reads as a family.
        character_visual(s, buddy, companion_character(options.character), 0.82f);
    }

    // A sentry turret run by a behaviour tree (assets/ai/sentry.vbt): it sweeps its head while
    // nothing is in sight and turns to track the player once its Perception sees it. The tree's
    // "scan" and "track" actions are Luau methods of scripts/sentry.luau.
    {
        const Entity sentry = add(s, "Sentry", {6.5f, 0, -6.0f});
        attach(s, sentry, "scripts/sentry.luau");
        auto& bt = s.world().add<ai::BehaviorTreeComponent>(sentry);
        bt.tree = "ai/sentry.vbt";
        auto& eyes = s.world().add<ai::PerceptionComponent>(sentry);
        eyes.sight_range = 9.0f;
        eyes.fov = 100.0f;
        eyes.eye_height = 1.45f;
        eyes.memory = 1.5f;
        const Entity base = add(s, "Base", {0, 0.6f, 0}, sentry);
        s.world().get<Transform>(base)->scale = {0.8f, 1.2f, 0.8f};
        mesh(s, base, "cylinder", {0.25f, 0.26f, 0.3f, 1}, 1.0f, 0.45f);
        s.world().add<physics::RigidBodyComponent>(base).type = physics::BodyType::Static;
        s.world().add<physics::ColliderComponent>(base).shape = physics::ColliderComponent::Shape::Cylinder;
        const Entity head = add(s, "Head", {0, 1.45f, 0.05f}, sentry);
        s.world().get<Transform>(head)->scale = {0.55f, 0.4f, 0.7f};
        mesh(s, head, "cube", {0.85f, 0.86f, 0.9f, 1}, 0.6f, 0.35f);
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

    {
        const Prefab crate = crate_prefab();
        Uuid asset;
        (void)Uuid::parse(kCratePrefabUuid, asset);
        for (int i = 0; i < 3; ++i) {
            InstantiateOptions o;
            o.name = "Crate " + std::to_string(i + 1);
            o.position = math::Vec3{3.6f + 0.05f * static_cast<float>(i), 0.3f + 0.62f * static_cast<float>(i), 2.0f};
            (void)instantiate(s, crate, asset, o);
        }
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

    (void)hud(s, "WASD, arrows or left stick   walk\nSpace or A button   jump\nE or X button   drop a crate (prefab)\nF5 / F9 or LB / RB   quick save / load\nWalk into the crates and the gate\nH or View button   show / hide this panel", {470, 178}, true, [] {
        json::Value p = json::Value::object(); // crate and gate impacts ring in the yard
        p.set("reverb_send", 0.18);
        return p;
    }());
}

// ---- Multiplayer arena ------------------------------------------------------------------------

void build_multiplayer(Scene& s, const TemplateOptions& options) {
    attach(s, camera_3d(s, {0, 9.0f, 11.0f}, {0, 0.6f, 0}), "scripts/net_camera.luau", [] {
        json::Value p = json::Value::object();
        p.set("offset", json::Array{0.0, 7.0, 9.0});
        return p;
    }());
    sun(s);
    {
        auto& env = s.world().add<gameplay::EnvironmentComponent>(add(s, "Environment", {0, 0, 0}));
        env.bloom_intensity = 0.05f;
        env.ao_intensity = 1.0f;
        env.shadow_distance = 34.0f;
    }
    const Entity ground = add(s, "Arena Floor", {0, 0, 0});
    s.world().get<Transform>(ground)->scale = {24, 1, 24};
    mesh(s, ground, "plane", {0.14f, 0.16f, 0.2f, 1}, 0.0f, 0.85f);
    {
        s.world().add<physics::RigidBodyComponent>(ground).type = physics::BodyType::Static;
        auto& c = s.world().add<physics::ColliderComponent>(ground);
        c.size = {1, 0.2f, 1};
        c.offset = {0, -0.1f, 0};
    }
    // Low walls around the 24 x 24 arena.
    const math::Color wall_color{0.32f, 0.36f, 0.45f, 1};
    const struct {
        const char* name;
        math::Vec3 at, size;
    } walls[] = {{"Wall North", {0, 0.5f, -12.25f}, {25, 1, 0.5f}},
                 {"Wall South", {0, 0.5f, 12.25f}, {25, 1, 0.5f}},
                 {"Wall West", {-12.25f, 0.5f, 0}, {0.5f, 1, 24}},
                 {"Wall East", {12.25f, 0.5f, 0}, {0.5f, 1, 24}}};
    for (const auto& w : walls) {
        const Entity e = add(s, w.name, w.at);
        s.world().get<Transform>(e)->scale = w.size;
        mesh(s, e, "cube", wall_color, 0.0f, 0.7f);
        s.world().add<physics::RigidBodyComponent>(e).type = physics::BodyType::Static;
        s.world().add<physics::ColliderComponent>(e);
    }
    // Four glowing pillars to run around.
    for (int i = 0; i < 4; ++i) {
        const float a = 0.7854f + 1.5708f * static_cast<float>(i);
        const Entity e = add(s, "Pillar " + std::to_string(i + 1), {std::cos(a) * 6.0f, 1.25f, std::sin(a) * 6.0f});
        s.world().get<Transform>(e)->scale = {1.1f, 2.5f, 1.1f};
        mesh(s, e, "cylinder", {0.9f, 0.9f, 0.95f, 1}, 1.0f, 0.25f);
        s.world().add<physics::RigidBodyComponent>(e).type = physics::BodyType::Static;
        s.world().add<physics::ColliderComponent>(e).shape = physics::ColliderComponent::Shape::Cylinder;
        const Entity lamp = add(s, "Pillar Light " + std::to_string(i + 1), {std::cos(a) * 6.0f, 2.9f, std::sin(a) * 6.0f});
        auto& l = s.world().add<LightComponent>(lamp);
        l.type = LightComponent::Type::Point;
        l.color = i % 2 ? math::Color{0.35f, 0.6f, 1.0f, 1} : math::Color{1.0f, 0.55f, 0.3f, 1};
        l.intensity = 9.0f;
        l.range = 6.5f;
    }

    // The player template: every client that joins a server gets its own copy (spawned around the
    // template's position). Offline (editor Play, `victus run`) it is simply the player.
    const Entity player = add(s, "Player", {0, 0, 2.0f});
    attach(s, player, "scripts/net_player.luau");
    s.world().add<net::NetworkIdentityComponent>(player).player_template = true;
    {
        auto& cc = s.world().add<physics::CharacterControllerComponent>(player);
        cc.radius = 0.6f;
        cc.height = 1.4f;
        cc.push_force = 200.0f;
    }
    character_visual(s, player, options.character, 1.45f);
    {
        const Entity badge = add(s, "Badge", {0, 2.05f, 0}, player);
        s.world().get<Transform>(badge)->scale = {0.32f, 0.32f, 0.32f};
        mesh(s, badge, "sphere", {0.3f, 0.8f, 1.0f, 1}, 0.0f, 0.3f);
    }

    // Coins: replicated server objects. Only the server (or offline play) collects them; the
    // collecting player's "score" network variable goes up and the coin hops elsewhere.
    for (int i = 0; i < 8; ++i) {
        const float a = 0.3927f + 0.7854f * static_cast<float>(i);
        const float r = i % 2 ? 9.0f : 3.6f;
        const Entity coin = add(s, "Coin " + std::to_string(i + 1), {std::cos(a) * r, 0.7f, std::sin(a) * r});
        s.world().get<Transform>(coin)->scale = {0.55f, 0.1f, 0.55f}; // a disc, stood up below
        mesh(s, coin, "cylinder", {1.0f, 0.78f, 0.2f, 1}, 1.0f, 0.25f);
        s.world().get<Transform>(coin)->rotation = math::Quat::axis_angle({1, 0, 0}, 1.5708f);
        s.world().add<physics::RigidBodyComponent>(coin).type = physics::BodyType::Kinematic;
        auto& c = s.world().add<physics::ColliderComponent>(coin);
        c.shape = physics::ColliderComponent::Shape::Sphere;
        c.radius = 1.0f;
        c.is_trigger = true;
        s.world().add<net::NetworkIdentityComponent>(coin);
        attach(s, coin, "scripts/net_coin.luau");
    }

    const Entity canvas = hud(s, "WASD, arrows or left stick   walk\nSpace or A button   jump\nCollect coins; the server keeps score\nH or View button   show / hide this panel", {470, 128});
    {
        const Entity status = ui_rect(s, canvas, "Net Status", {0, 0}, {0, 0}, {24, 24}, {520, 40});
        auto& t = ui_text(s, status, "Offline", 22, true);
        t.align = gameui::TextAlign::Left;
        const Entity board = ui_rect(s, canvas, "Scoreboard", {0, 0}, {0, 0}, {24, 72}, {360, 220});
        auto& bp = s.world().add<gameui::UiPanelComponent>(board);
        bp.border_width = 1.5f;
        // Scroll view: net_hud.luau sizes the text to its rows, so a long player list scrolls
        // (mouse wheel or drag over the board).
        s.world().add<gameui::UiScrollComponent>(board).wheel_step = 28;
        gameui::UiRectComponent inner;
        inner.anchor_min = {0, 0};
        inner.anchor_max = {1, 0};
        inner.pivot = {0.5f, 0};
        inner.position = {0, 12};
        inner.size = {-32, 196};
        const Entity body = s.create("Scoreboard Text", board);
        s.world().add<gameui::UiRectComponent>(body, inner);
        auto& bt = ui_text(s, body, "", 22, false);
        bt.align = gameui::TextAlign::Left;
        bt.vertical_align = gameui::VerticalAlign::Top;
        attach(s, board, "scripts/net_hud.luau");
    }
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

    // HUD: coin counter (updated by platformer.luau), a hidden win banner and the controls panel.
    const Entity canvas = hud(s, "A / D, arrows or left stick   run\nSpace, W, Up or A button   jump\nS, Down or stick down   drop through the ledge\nF5 / F9 or LB / RB   quick save / load\nH or View button   show / hide this panel",
                              {470, 156}, true);
    const Entity counter = ui_rect(s, canvas, "Coin Counter", {0, 0}, {0, 0}, {24, 24}, {236, 60});
    s.world().add<gameui::UiPanelComponent>(counter).border_width = 1.5f;
    ui_text(s, counter, "Coins 0 / 3", 28, true).color = {1.0f, 0.86f, 0.35f, 1};
    const Entity banner = ui_rect(s, canvas, "Win Banner", {0.5f, 0}, {0.5f, 0}, {0, 120}, {560, 88});
    auto& bp = s.world().add<gameui::UiPanelComponent>(banner);
    bp.color = {0.12f, 0.09f, 0.03f, 0.85f};
    bp.border_color = {1.0f, 0.8f, 0.3f, 0.9f};
    bp.border_width = 2.5f;
    bp.corner_radius = 16;
    ui_text(s, banner, "All coins collected!", 40, true).color = {1.0f, 0.86f, 0.35f, 1};
    s.world().get<gameui::UiRectComponent>(banner)->visible = false;
}

// "No-Code Platformer": a complete level built only from Behaviours (ADR-0120): no scripts at
// all, so every part of the game is a component a beginner can click on and change. The sprites
// are drawn in code (sprite_art.cpp) and written into the project as ordinary PNG assets.
SpriteComponent& art(SpriteComponent& sp, const char* name) {
    (void)Uuid::parse(detail::nocode_sprite_uuid(name), sp.texture_asset);
    return sp;
}

void build_nocode(Scene& s) {
    using namespace script;
    constexpr math::Color white{1, 1, 1, 1};
    const Entity cam = add(s, "Main Camera", {-6, -1, 10});
    auto& c = s.world().add<CameraComponent>(cam);
    c.projection = CameraComponent::Projection::Orthographic;
    c.ortho_height = 10.0f;
    auto& follow = s.world().add<CameraFollowComponent>(cam);
    follow.offset = {2.5f, 1.8f, 10};
    follow.smoothing = 5;
    follow.look_at = false;
    // The sky and sun ride along with the camera so they always fill the view.
    art(sprite(s, add(s, "Sky", {0, 0, -11}, cam), {40, 14}, white, -10), "sky");
    art(sprite(s, add(s, "Sun", {5.8f, 3.0f, -10.5f}, cam), {1.8f, 1.8f}, {1.0f, 0.9f, 0.45f, 1}, -9), "round").unlit = true;

    // Scenery: clouds, distant hills and bushes. Sprites only, nothing collides with them.
    const struct { float x, y, w; } clouds[] = {{-5, 1.4f, 3.2f}, {6, 2.2f, 2.4f}, {16, 1.2f, 3.6f}, {28, 2.4f, 2.6f},
                                                {39, 1.6f, 3.4f}, {50, 2.3f, 2.8f}, {60, 1.3f, 3.0f}};
    int k = 0;
    for (const auto& cl : clouds)
        art(sprite(s, add(s, "Cloud " + std::to_string(++k), {cl.x, cl.y, -0.9f}), {cl.w, cl.w * 0.5f}, {1, 1, 1, 0.92f}, -8),
            "cloud");
    const struct { float x, r; } hills[] = {{-6, 7}, {6, 5}, {16, 8}, {30, 6}, {42, 7.5f}, {55, 6}};
    k = 0;
    for (const auto& hl : hills)
        art(sprite(s, add(s, "Hill " + std::to_string(++k), {hl.x, -3.4f, -0.8f}), {hl.r * 2, hl.r * 1.3f},
                   {0.4f, 0.66f, 0.42f, 1}, -7),
            "round");
    const float bushes[] = {-3, 3, 12.5f, 24, 34, 47, 54};
    k = 0;
    for (const float x : bushes)
        art(sprite(s, add(s, "Bush " + std::to_string(++k), {x, -2.85f, -0.3f}), {1.6f, 0.9f}, {0.2f, 0.5f, 0.24f, 1}, -1),
            "round");

    const math::Color grass{0.36f, 0.72f, 0.3f, 1}, dirt{0.5f, 0.32f, 0.2f, 1}, wood{0.62f, 0.42f, 0.24f, 1};
    const struct { const char* name; float x, w; } grounds[] = {{"Ground", 2, 24}, {"Ground 2", 28.5f, 23}, {"Ground 3", 51.5f, 17}};
    for (const auto& g : grounds) {
        const Entity e = solid_2d(s, g.name, {g.x, -6, 0}, {g.w, 6}, dirt, 0.6f);
        sprite(s, add(s, "Grass", {0, 2.85f, 0}, e), {g.w, 0.3f}, grass, 1);
    }
    solid_2d(s, "Wall Left", {-10.5f, 2, 0}, {1, 18}, dirt, 0.0f);
    solid_2d(s, "Wall Right", {60.5f, 2, 0}, {1, 18}, dirt, 0.0f);
    const struct { const char* name; float x, y; bool one_way; } platforms[] = {
        {"Platform", 6, -0.6f, true}, {"Platform 2", 22, -0.4f, false}, {"Platform 3", 25.5f, 1.6f, false}, {"Platform 4", 46, -0.6f, false}};
    for (const auto& pl : platforms) {
        const Entity e = solid_2d(s, pl.name, {pl.x, pl.y, 0}, {3, 0.4f}, wood, 0.6f);
        sprite(s, add(s, "Grass", {0, 0.17f, 0}, e), {3, 0.12f}, grass, 1);
        if (pl.one_way) s.world().get<physics::Collider2DComponent>(e)->one_way = true;
    }

    // The player: four behaviours and a physics character, nothing else.
    const Entity player = add(s, "Player", {-6, -2.5f, 0});
    art(sprite(s, player, {0.9f, 0.9f}, white, 3), "player");
    auto& pc = s.world().add<physics::CharacterController2DComponent>(player);
    pc.size = {0.86f, 0.9f};
    pc.gravity_scale = 2.6f;
    pc.air_control = 0.8f;
    auto& move = s.world().add<PlatformerMovementComponent>(player);
    move.speed = 7;
    move.jump_speed = 12.5f;
    move.max_jumps = 2;
    move.jump_sound = "audio/jump.wav";
    auto& hp = s.world().add<HealthComponent>(player);
    hp.on_death = DeathAction::Respawn;
    auto& gun = s.world().add<ShooterComponent>(player);
    gun.speed = 16;
    gun.cooldown = 0.25f;

    const math::Vec3 coins[] = {{0, -2.3f, 0},   {6, 0.3f, 0},     {15.5f, -0.6f, 0}, {22, 0.5f, 0},
                                {25.5f, 2.6f, 0}, {33, -2.3f, 0},   {46, 0.3f, 0},     {52, -2.3f, 0}};
    int n = 0;
    for (const math::Vec3& at : coins) {
        const Entity coin = add(s, "Coin " + std::to_string(++n), at);
        art(sprite(s, coin, {0.5f, 0.5f}, white, 2), "coin").unlit = true;
        s.world().add<OscillatorComponent>(coin) = {{0, 0.15f, 0}, 0.8f, static_cast<float>(n) * 0.4f, {}, false};
        s.world().add<CollectibleComponent>(coin).sound = "audio/coin.wav";
    }
    const Entity heart = add(s, "Heart", {28, -2.3f, 0});
    art(sprite(s, heart, {0.55f, 0.55f}, white, 2), "heart");
    s.world().add<OscillatorComponent>(heart) = {{0, 0.12f, 0}, 0.6f, 0, {}, false};
    auto& heal = s.world().add<CollectibleComponent>(heart);
    heal.kind = CollectibleKind::Health;
    heal.sound = "audio/coin.wav";

    for (const float x : {10.0f, 37.0f}) {
        const Entity spikes = add(s, x < 20 ? "Spikes" : "Spikes 2", {x, -2.75f, 0});
        art(sprite(s, spikes, {1.2f, 0.5f}, white, 2), "spikes");
        s.world().add<HazardComponent>(spikes);
    }
    const struct { const char* name; float x, offset; } slimes[] = {{"Slime", 31, -4}, {"Slime 2", 49, 3}};
    for (const auto& sl : slimes) {
        const Entity slime = add(s, sl.name, {sl.x, -2.6f, 0});
        art(sprite(s, slime, {0.9f, 0.8f}, white, 3), "slime");
        auto& hz = s.world().add<HazardComponent>(slime);
        hz.stompable = true;
        auto& patrol = s.world().add<PatrolComponent>(slime);
        patrol.offset = {sl.offset, 0, 0};
        patrol.speed = 1.6f;
        patrol.turn_at_ledges = true; // never walks into a pit
        auto& life = s.world().add<HealthComponent>(slime);
        life.max = life.current = 1;
        life.invulnerable_time = 0;
    }
    // Falling into a gap costs a life: a long invisible trigger below the level.
    const Entity pit = add(s, "Pit", {25, -11, 0});
    auto& pit_shape = s.world().add<physics::Collider2DComponent>(pit);
    pit_shape.size = {120, 2};
    pit_shape.is_trigger = true;
    auto& pit_hz = s.world().add<HazardComponent>(pit);
    pit_hz.instant_kill = true;
    pit_hz.knockback = 0;

    const Entity checkpoint = add(s, "Checkpoint", {19.5f, -2.3f, 0});
    art(sprite(s, checkpoint, {0.56f, 1.4f}, {0.45f, 0.7f, 1.0f, 1}, 2), "flag");
    s.world().add<CheckpointComponent>(checkpoint);
    const Entity goal = add(s, "Goal Flag", {57, -2.0f, 0});
    art(sprite(s, goal, {0.8f, 2.0f}, {1.0f, 0.84f, 0.3f, 1}, 2), "flag").unlit = true;
    s.world().add<GoalComponent>(goal);

    const Entity rules = add(s, "Game Rules", {0, 0, 0});
    auto& gr = s.world().add<GameRulesComponent>(rules);
    gr.lives = 3;
    gr.win_message = "Level complete!";

    const Entity music = add(s, "Music", {0, 0, 0});
    auto& ms = s.world().add<audio::AudioSourceComponent>(music);
    ms.clip = "audio/music.wav";
    ms.bus = audio::Bus::Music;
    ms.volume = 0.3f;
    ms.loop = true;
    ms.stream = true;
    ms.spatial = false;

    // A static line of help along the bottom; GameRules adds score, lives and time at the top.
    const Entity canvas = s.create("Help");
    s.world().add<gameui::UiCanvasComponent>(canvas);
    const Entity help = ui_rect(s, canvas, "Controls", {0.5f, 1}, {0.5f, 1}, {0, -18}, {980, 44});
    auto& panel = s.world().add<gameui::UiPanelComponent>(help);
    panel.corner_radius = 12;
    ui_text(s, help, "A / D run     Space jump, again in the air to double jump     Click shoot     land on slimes to stomp them", 22, false);
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

constexpr const char* kPlayer3d = R"lua(-- Walks the character with the "move_x"/"move_y" actions (WASD, arrows, left stick) and jumps
-- with "jump" (Space, gamepad A). The entity has a
-- CharacterController (it slides along walls, climbs steps and pushes physics bodies); the
-- child "Robot" (a Vanguard starter character) faces the direction of travel; its animation
-- controller (assets/anim/characters/vanguard_locomotion.vanim) blends Idle/Walk/Run from the
-- "speed" parameter, plays Jump, Fall and Land from the "jump" trigger and "grounded", and
-- Interact from the "interact" trigger. Its child "Dust" particle emitter puffs on jumps and
-- landings.
-- Fields below are defaults; override them per entity in the editor inspector.
local Player = {}
Player.speed = 5
Player.jump_speed = 6
Player.bounds = 9 -- stay within this distance of the origin on X and Z
Player.crate = "prefabs/crate.vprefab" -- dropped with "interact" (E, gamepad X)
Player.max_crates = 8                  -- the oldest dropped crate is removed beyond this

function Player:start()
    self.robot = self.entity:find_child("Robot")
    self.dust = self.entity:find_child("Dust")
    self.air_time = 0
    self.dropped = {}
    self.facing = vector.create(0, 0, 1)
end

-- Drops an instance of the crate prefab in front of the player.
function Player:drop_crate()
    local p = self.entity.position + self.facing * 1.4 + vector.create(0, 1.6, 0)
    local crate = scene.instantiate(self.crate, { position = p, name = "Dropped Crate" })
    table.insert(self.dropped, crate)
    if #self.dropped > self.max_crates then
        scene.destroy(table.remove(self.dropped, 1))
    end
end

-- Checkpoints (quick save): entities in the returned table are stored by id.
function Player:save_state()
    return { dropped = self.dropped, facing = self.facing }
end

function Player:load_state(state)
    self.dropped = {}
    for _, crate in state.dropped or {} do
        if crate then table.insert(self.dropped, crate) end
    end
    if state.facing then self.facing = state.facing end
end

function Player:update(dt)
    local interacted = input.action_pressed("interact")
    if interacted then self:drop_crate() end
    -- Actions work with WASD/arrows and a gamepad's left stick (partial tilt walks slower).
    local stick = input.vector("move_x", "move_y")
    local move = vector.create(stick.x, 0, -stick.y) * self.speed
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
    local jumped = input.action_pressed("jump") and grounded
    if jumped then
        self.entity:jump(self.jump_speed)
        if self.dust then self.dust:emit_particles(14) end
    end
    -- Landing puff: grounded again after being airborne.
    if grounded and self.was_grounded == false and self.dust then self.dust:emit_particles(10) end
    self.was_grounded = grounded
    if not self.robot then return end
    -- Face the direction actually travelled (the robot model looks down +Z) and feed the
    -- animation controller: ground speed picks the Idle/Walk/Run blend, "jump" plays the take-off.
    local v = self.entity.velocity
    local speed = vector.magnitude(vector.create(v.x, 0, v.z))
    if speed > 0.3 then
        self.facing = vector.normalize(vector.create(v.x, 0, v.z))
        self.robot.rotation = vector.create(0, math.deg(math.atan2(v.x, v.z)), 0)
    end
    -- Short hops off step edges are not falls: report airborne only after 0.15 s off the ground.
    self.air_time = grounded and 0 or self.air_time + dt
    self.robot:set_anim_param("speed", speed)
    self.robot:set_anim_param("grounded", self.air_time < 0.15)
    if jumped then self.robot:anim_trigger("jump") end
    if interacted then self.robot:anim_trigger("interact") end
end

return Player
)lua";

constexpr const char* kCompanion = R"lua(-- A companion that follows `target` over the navmesh. The entity has a NavAgent and a
-- CharacterController: set_destination() plans a path around the steps, crates and pillars and
-- the character mover walks it. Its child "Robot" model's animation controller blends Idle, Walk
-- and Run by the "speed" parameter.
local Buddy = {}
Buddy.target = "Player"
Buddy.follow_distance = 2.0 -- stays about this far from the target
Buddy.repath_distance = 0.75 -- re-plans when the target moved this far

function Buddy:start()
    self.subject = scene.find(self.target)
    self.robot = self.entity:find_child("Robot")
end

function Buddy:update(dt)
    if self.subject and self.subject.valid then
        local goal = self.subject.position
        local here = self.entity.position
        local offset = vector.create(goal.x - here.x, 0, goal.z - here.z)
        local distance = vector.magnitude(offset)
        if distance > self.follow_distance then
            -- Walk to a point short of the target so the two never bump into each other.
            local spot = goal - vector.normalize(offset) * (self.follow_distance * 0.75)
            if not self.goal or vector.magnitude(spot - self.goal) > self.repath_distance then
                if self.entity:set_destination(spot) then self.goal = spot end
            end
        elseif self.goal and distance < self.follow_distance * 0.6 then
            self.entity:stop()
            self.goal = nil
        end
    end
    if not self.robot then return end
    local v = self.entity.velocity
    -- The companion is about half size: its legs cover half the ground per step.
    self.robot:set_anim_param("speed", vector.magnitude(vector.create(v.x, 0, v.z)) * 1.8)
end

return Buddy
)lua";

constexpr const char* kSentryTree = R"({
  "root": {"type": "selector", "reactive": true, "children": [
    {"type": "sequence", "name": "Engage", "children": [
      {"type": "check", "key": "target", "op": "set"},
      {"type": "action", "action": "track"}]},
    {"type": "action", "name": "Patrol", "action": "scan"}]}
}
)";

constexpr const char* kSentry = R"lua(-- A sentry turret driven by assets/ai/sentry.vbt. The behaviour tree calls these methods as
-- tasks (returning "running", "success" or "failure"): scan sweeps the turret while nothing is in
-- sight; once its Perception writes a `target` to the blackboard, track turns to face it.
local Sentry = {}
Sentry.sweep = 50 -- degrees either side of the starting heading
Sentry.scan_speed = 35 -- degrees per second while scanning
Sentry.turn_speed = 220 -- degrees per second while tracking

function Sentry:start()
    self.home = self.entity.rotation.y
    self.yaw = self.home
    self.dir = 1
    self.head = self.entity:find_child("Head")
end

function Sentry:face(yaw)
    self.yaw = yaw
    self.entity.rotation = vector.create(0, yaw, 0)
end

function Sentry:scan(dt, first)
    if first and self.head then self.head:set_emissive(vector.create(0.2, 0.6, 1.0), 1.5) end
    if self.yaw > self.home + self.sweep then self.dir = -1 elseif self.yaw < self.home - self.sweep then self.dir = 1 end
    self:face(self.yaw + self.dir * self.scan_speed * dt)
    return "running"
end

function Sentry:track(dt, first)
    local target = self.entity:bb_get("target")
    if not target or not target.valid then return "failure" end
    if first and self.head then self.head:set_emissive(vector.create(1.0, 0.15, 0.1), 3) end
    -- Perception looks along +Z, so the heading toward the target is atan2(dx, dz).
    local d = target.position - self.entity.position
    local want = math.deg(math.atan2(d.x, d.z))
    local diff = ((want - self.yaw + 540) % 360) - 180
    local step = self.turn_speed * dt
    self:face(self.yaw + math.clamp(diff, -step, step))
    return "running"
end

return Sentry
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
    self:show_score()
end

-- Checkpoints (quick save): collected coins are already gone from the saved scene.
function Player:save_state()
    return { score = self.score, total = self.total, spawn = self.spawn }
end

function Player:load_state(state)
    self.score = state.score or 0
    self.total = state.total or self.total
    self.spawn = state.spawn or self.spawn
    self:show_score()
end

-- The HUD counter (a UiText on the "Coin Counter" panel).
function Player:show_score()
    local counter = scene.find("Coin Counter")
    if counter then counter:set_text("Coins " .. self.score .. " / " .. self.total) end
end

function Player:fixed_update(dt)
    local e = self.entity
    e:move_character(vector.create(input.value("move_x") * self.speed, 0, 0))
    local vertical = input.value("move_y")
    if vertical <= -0.5 and e.grounded then
        e:drop_through()
    elseif (input.action("jump") or vertical >= 0.5) and e:jump(self.jump_speed) then
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
        self:show_score()
        if self.score == self.total then
            log.info("All coins collected!")
            local banner = scene.find("Win Banner")
            if banner then banner:set_visible(true) end
            audio.set_lowpass("Music", 900) -- the level is done: muffle the music under the banner
        end
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

constexpr const char* kHud = R"lua(-- HUD canvas: clicks on its buttons bubble up here as on_click(button, action). The
-- "Controls" button (or H) shows and hides the controls panel. When the HUD has a "Save Status"
-- toast, "quick_save" (F5, LB) writes a checkpoint of the whole scene to the "quick" slot and
-- "quick_load" (F9, RB) restores it; every script's save_state/load_state carries its own data.
-- Messages come from assets/localization/<locale>.vstrings (loc.text); tr() keeps the English
-- text when a key is missing.
local Hud = {}
Hud.slot = "quick"

local function tr(key, fallback, params)
    if loc.has(key) then return loc.text(key, params) end
    return fallback
end
Hud.toast_seconds = 2
-- Room sound: the share of Sfx sent to the engine's reverb (0 = dry) and how long it rings.
Hud.reverb_send = 0
Hud.reverb_decay = 0.6

function Hud:start()
    self.toast = 0
    if self.reverb_send > 0 then
        audio.set_reverb_send("Sfx", self.reverb_send)
        audio.set_reverb_decay(self.reverb_decay)
    end
end

function Hud:notify(text)
    local t = scene.find("Save Status")
    if not t then return end
    t:set_text(text)
    t:set_visible(true)
    self.toast = self.toast_seconds
end

function Hud:quick_save()
    local ok, err = save.checkpoint(self.slot, "Quick save")
    self:notify(if ok then tr("hud.saved", "Game saved") else tr("hud.save_failed", "Save failed: " .. tostring(err), { error = tostring(err) }))
end

function Hud:quick_load()
    if not save.exists(self.slot) then
        self:notify(tr("hud.no_save", "No quick save yet (F5 saves)"))
        return
    end
    local ok, err = save.restore(self.slot) -- the scene is replaced at the end of this frame
    if not ok then self:notify(tr("hud.load_failed", "Load failed: " .. tostring(err), { error = tostring(err) })) end
end

-- Checkpoint hooks: the restored HUD confirms the load.
function Hud:save_state()
    return { saved = true }
end

function Hud:load_state(state)
    self:notify(tr("hud.loaded", "Game loaded"))
end

function Hud:toggle_help()
    local panel = scene.find("Controls Panel")
    if panel then panel:set_visible(not panel:visible()) end
end

function Hud:on_click(button, action)
    if action == "toggle_help" then self:toggle_help() end
end

function Hud:update(dt)
    if input.action_pressed("help") then self:toggle_help() end
    if scene.find("Save Status") then
        if input.action_pressed("quick_save") then self:quick_save() end
        if input.action_pressed("quick_load") then self:quick_load() end
    end
    if self.toast > 0 then
        self.toast -= dt
        if self.toast <= 0 then
            local t = scene.find("Save Status")
            if t then t:set_visible(false) end
        end
    end
end

return Hud
)lua";

// English strings of the HUD (assets/localization/en.vstrings); copy the file to fr.vstrings,
// translate it and call loc.set_locale("fr") to switch.
constexpr const char* kHudStrings = R"json({
  "name": "English",
  "strings": {
    "hud": {
      "controls": "Controls",
      "saved": "Game saved",
      "save_failed": "Save failed: {error}",
      "no_save": "No quick save yet (F5 saves)",
      "load_failed": "Load failed: {error}",
      "loaded": "Game loaded"
    }
  }
}
)json";

constexpr const char* kNetPlayer = R"lua(-- Multiplayer player. On a server every joined client gets a copy of this entity; the server
-- runs it with that client's input and replicates the result, while the owning client predicts
-- it locally (net.authority() or is_local()). Other players are proxies on each client: they
-- only run the cosmetics below (facing, animation, badge colour). Offline it is a normal player.
-- "score" and "color" are network variables: set by the server, readable everywhere.
local Player = {}
Player.speed = 5.5
Player.jump_speed = 6

local COLORS = {
    { 0.3, 0.8, 1.0 }, { 1.0, 0.45, 0.35 }, { 0.45, 1.0, 0.5 }, { 1.0, 0.85, 0.3 },
    { 0.8, 0.5, 1.0 }, { 1.0, 0.6, 0.85 }, { 0.5, 1.0, 0.95 }, { 0.95, 0.95, 0.95 },
}

function Player:start()
    self.robot = self.entity:find_child("Robot")
    self.badge = self.entity:find_child("Badge")
    self.last = self.entity.position
    self.spawn = self.entity.position
    if net.authority() then
        self.entity:set_net("score", 0)
        self.entity:set_net("color", self.entity:owner() % #COLORS + 1)
    end
end

function Player:update(dt)
    local e = self.entity
    if net.authority() or e:is_local() then
        local stick = input.vector("move_x", "move_y")
        e:move_character(vector.create(stick.x, 0, -stick.y) * self.speed)
        if input.action_pressed("jump") and e.grounded then e:jump(self.jump_speed) end
        if net.authority() and e.position.y < -10 then e.position = self.spawn end
    end

    -- Cosmetics on every machine, from how far the entity actually moved this frame.
    local p = e.position
    local d = vector.create(p.x - self.last.x, 0, p.z - self.last.z)
    self.last = p
    local speed = dt > 0 and vector.magnitude(d) / dt or 0
    if self.robot then
        if speed > 0.3 then self.robot.rotation = vector.create(0, math.deg(math.atan2(d.x, d.z)), 0) end
        self.robot:set_anim_param("speed", speed) -- assets/anim/characters/vanguard_locomotion.vanim blends Idle/Walk/Run
    end
    local color = e:get_net("color")
    if self.badge and color and color ~= self.color then
        self.color = color
        local c = COLORS[color] or COLORS[1]
        self.badge:set("MeshRenderer", { base_color = { c[1], c[2], c[3], 1 } })
    end
end

function Player:on_trigger_enter(other)
    if not net.authority() or string.sub(other.name, 1, 4) ~= "Coin" then return end
    self.entity:set_net("score", (self.entity:get_net("score") or 0) + 1)
    other:set_net("hops", (other:get_net("hops") or 0) + 1) -- the coin moves itself elsewhere
end

return Player
)lua";

constexpr const char* kNetCoin = R"lua(-- A replicated coin. The server (or offline play) spins and hovers it and, when a player
-- collected it (its "hops" network variable changed), moves it somewhere else in the arena.
-- Clients only see the replicated transform.
local Coin = {}
Coin.spin_speed = 180
Coin.hover = 0.15
Coin.area = 10 -- relocates within +-area on X and Z

function Coin:start()
    self.base = self.entity.position
    self.t = math.random() * 6
    self.hops = self.entity:get_net("hops") or 0
end

-- A random spot in the arena at least `clearance` away from every player (so a coin never
-- reappears under someone who is standing still).
Coin.clearance = 2.5
function Coin:free_spot()
    local spot = self.base
    for _ = 1, 16 do
        spot = vector.create((math.random() * 2 - 1) * self.area, self.base.y, (math.random() * 2 - 1) * self.area)
        local clear = true
        for _, p in scene.query("CharacterController") do
            local d = p.position - spot
            if d.x * d.x + d.z * d.z < self.clearance * self.clearance then
                clear = false
                break
            end
        end
        if clear then return spot end
    end
    return spot
end

function Coin:update(dt)
    if not net.authority() then return end
    local hops = self.entity:get_net("hops") or 0
    if hops ~= self.hops then
        self.hops = hops
        self.base = self:free_spot()
    end
    self.t += dt
    self.entity:rotate(vector.create(0, 0, 1), self.spin_speed * dt)
    self.entity.position = self.base + vector.create(0, math.sin(self.t * 3) * self.hover, 0)
end

return Coin
)lua";

constexpr const char* kNetCamera = R"lua(-- Follows this machine's player: the predicted local player on a client, otherwise the entity
-- named `target`.
local Cam = {}
Cam.target = "Player"
Cam.offset = vector.create(0, 7, 9)
Cam.smoothing = 5

function Cam:update(dt)
    local subject = net.local_player() or scene.find(self.target) -- client or listen-server host
    if not subject or not subject.valid then return end
    local goal = subject.position + self.offset
    local k = 1 - math.exp(-self.smoothing * dt)
    self.entity.position = self.entity.position + (goal - self.entity.position) * k
    self.entity:look_at(subject.position + vector.create(0, 0.8, 0))
end

return Cam
)lua";

constexpr const char* kNetHud = R"lua(-- Session status (role, connection, round trip) and the scoreboard, refreshed a few times
-- per second from net.stats(), net.clients() and each player's "score" network variable.
-- The scoreboard is a UiScroll view: the text grows with the rows and scrolls past the board.
local Board = {}
Board.interval = 0.25
Board.line_height = 28 -- canvas units per row at the board's 22 px font

function Board:start()
    self.status = scene.find("Net Status")
    self.text = scene.find("Scoreboard Text")
    self.wait = 0
end

function Board:update(dt)
    self.wait -= dt
    if self.wait > 0 then return end
    self.wait = self.interval
    local st = net.stats()
    local line
    if st.role == "client" then
        if net.connected() then
            line = string.format("Online   player %d   %d players   ping %.0f ms", net.id(), #net.clients(), st.rtt_ms)
        else
            line = "Client   " .. st.status
        end
    elseif st.role == "server" then
        if net.local_player() then
            line = string.format("Hosting   %d players", #net.clients())
        else
            line = string.format("Server   %d players", st.clients)
        end
    else
        line = "Offline   (host with: victus_player --host or victus serve)"
    end
    if self.status then self.status:set_text(line) end
    local rows = {}
    local clients = net.clients()
    if #clients == 0 then
        local p = scene.find("Player")
        if p then table.insert(rows, string.format("You   %d", p:get_net("score") or 0)) end
    else
        for _, c in clients do
            local score = (c.player and c.player.valid) and (c.player:get_net("score") or 0) or 0
            local me = c.id == net.id() and "  (you)" or ""
            table.insert(rows, string.format("%s   %d%s", c.name, score, me))
        end
    end
    if self.text then
        self.text:set_text("Scores\n" .. table.concat(rows, "\n"))
        self.text:set("UiRect", { size = { -32, (#rows + 1) * self.line_height + 12 } })
    end
end

return Board
)lua";

} // namespace

const std::vector<TemplateInfo>& templates() {
    static const std::vector<TemplateInfo> list = {
        {"3d", "3D Scene", "A Vanguard starter robot you walk with WASD/arrow keys, jump with Space and interact with E (Luau + CharacterController) that climbs steps, swings a hinged gate and shoves a stack of physics crates with positional impact sounds, its Vanguard counterpart as a companion that follows you over an automatic navmesh, a follow camera, sun shadows, PBR primitives and a point light.", true},
        {"2d", "2D Game", "A playable platformer on Box2D physics with music and sound effects: a CharacterController2D you run with A/D or arrows and jump with Space, a one-way ledge you jump through and drop off with S/Down, a lantern swinging on a joint and coins to collect. Gameplay is in Luau scripts."},
        {"nocode", "No-Code Platformer",
         "A complete side-scrolling level made without a single line of code: run, double jump, collect coins, stomp patrolling slimes, dodge spikes and pits, reach a checkpoint and the goal flag, with score, lives and a timer on screen. Every piece is a Behaviour component you can click on and change in the Inspector."},
        {"simulation", "Simulation",
         "576 entities animated by native systems, with a scripted orbiting camera: a starting point for simulation work."},
        {"multiplayer", "Multiplayer Arena",
         "A networked coin-collecting arena: host it with `victus serve` and join with `victus run --connect` (or play it offline). Each client gets its own predicted Vanguard robot from the player template, coins and scores are replicated server state, and the HUD shows the session status and scoreboard.", true},
    };
    return list;
}

std::vector<TemplateFile> template_files(std::string_view id, const TemplateOptions& options) {
    if (id == "3d") {
        std::vector<TemplateFile> files = {{"assets/scripts/player.luau", kPlayer3d},
                {"assets/scripts/follow_camera.luau", kFollowCamera},
                {"assets/scripts/companion.luau", kCompanion},
                {"assets/scripts/sentry.luau", kSentry},
                {"assets/ai/sentry.vbt", kSentryTree},
                {"assets/scripts/crate.luau", kCrate},
                {kCratePrefabPath, json::stringify(prefab_to_json(crate_prefab())) + "\n"},
                {std::string(kCratePrefabPath) + ".meta", crate_prefab_meta()},
                {"assets/audio/impact.wav", synth_impact()},
                {kGroundTexturePath, ground_texture()},
                {std::string(kGroundTexturePath) + ".meta", asset_meta(kGroundTextureUuid, "texture")},
                {kGroundMaterialPath, ground_material()},
                {std::string(kGroundMaterialPath) + ".meta", asset_meta(kGroundMaterialUuid, "")},
                {kEnergyShaderPath, kEnergyShader},
                {std::string(kEnergyShaderPath) + ".meta", asset_meta(kEnergyShaderUuid, "")},
                {kEnergyMaterialPath, energy_material()},
                {std::string(kEnergyMaterialPath) + ".meta", asset_meta(kEnergyMaterialUuid, "")},
                {"assets/scripts/hud.luau", kHud},
                {"assets/localization/en.vstrings", kHudStrings}};
        add_character_files(files, options);
        return files;
    }
    if (id == "2d")
        return {{"assets/scripts/platformer.luau", kPlatformer},
                {"assets/scripts/coin.luau", kCoin},
                {"assets/audio/jump.wav", synth_jump()},
                {"assets/audio/coin.wav", synth_coin()},
                {"assets/audio/music.wav", synth_music()},
                {"assets/scripts/hud.luau", kHud},
                {"assets/localization/en.vstrings", kHudStrings}};
    if (id == "nocode") {
        std::vector<TemplateFile> files{
            {"assets/audio/jump.wav", synth_jump()}, {"assets/audio/coin.wav", synth_coin()}, {"assets/audio/music.wav", synth_music()}};
        for (auto& a : detail::nocode_sprite_art()) {
            const std::string path = std::string("assets/textures/nocode/") + a.name + ".png";
            files.push_back({path + ".meta", asset_meta(a.uuid, "texture")});
            files.push_back({path, std::move(a.png)});
        }
        return files;
    }
    if (id == "simulation") return {{"assets/scripts/orbit_camera.luau", kOrbitCamera}};
    if (id == "multiplayer") {
        std::vector<TemplateFile> files = {{"assets/scripts/net_player.luau", kNetPlayer},
                                           {"assets/scripts/net_coin.luau", kNetCoin},
                                           {"assets/scripts/net_camera.luau", kNetCamera},
                                           {"assets/scripts/net_hud.luau", kNetHud},
                                           {"assets/scripts/hud.luau", kHud},
                                           {"assets/localization/en.vstrings", kHudStrings}};
        add_character_files(files, options);
        return files;
    }
    return {};
}

void register_components() {
    gameplay::register_components();
    script::register_components();
    physics::register_components();
    audio::register_components();
    animation::register_components();
    particles::register_components();
    gameui::register_components();
    navigation::register_components();
    ai::register_components();
    sequencer::register_components();
    terrain::register_components();
    net::register_components();
    tilemap::register_components();
    world::register_components();
}

Status build_template_scene(std::string_view id, Scene& s, const TemplateOptions& options) {
    s.clear();
    if (id == "3d") build_3d(s, options);
    else if (id == "2d") build_2d(s);
    else if (id == "nocode") build_nocode(s);
    else if (id == "simulation") build_simulation(s);
    else if (id == "multiplayer") build_multiplayer(s, options);
    else return make_error(ErrorCode::InvalidArgument, "unknown template '" + std::string(id) + "'");
    s.update_transforms();
    return {};
}

} // namespace victus::project
