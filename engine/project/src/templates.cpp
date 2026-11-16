// SPDX-License-Identifier: Apache-2.0
// Built-in project templates. Each produces a scene that runs immediately (Play) with the
// built-in behaviours, so a new project shows motion, lighting and a camera on first launch.
#include "victus/gameplay/gameplay.hpp"
#include "victus/project/project.hpp"
#include "victus/script/script.hpp"

#include <algorithm>
#include <cmath>

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
    const Entity ground = add(s, "Ground", {0, 0, 0});
    s.world().get<Transform>(ground)->scale = {20, 1, 20};
    mesh(s, ground, "plane", {0.16f, 0.17f, 0.19f, 1}, 0.0f, 0.9f);

    const Entity cube = add(s, "Spinning Cube", {-2.2f, 0.75f, 0});
    s.world().get<Transform>(cube)->scale = {1.5f, 1.5f, 1.5f};
    mesh(s, cube, "cube", {0.9f, 0.32f, 0.08f, 1}, 0.0f, 0.45f);
    s.world().add<RotatorComponent>(cube) = {{0, 1, 0}, 40.0f};

    const Entity sphere = add(s, "Player", {0, 1.2f, 0.5f});
    s.world().get<Transform>(sphere)->scale = {1.4f, 1.4f, 1.4f};
    mesh(s, sphere, "sphere", {0.1f, 0.35f, 0.9f, 1}, 0.0f, 0.25f);
    attach(s, sphere, "scripts/player.luau");

    const Entity bob = add(s, "Floating Gem", {1.0f, 2.6f, -2.0f});
    s.world().get<Transform>(bob)->scale = {0.5f, 0.5f, 0.5f};
    mesh(s, bob, "cube", {0.2f, 0.85f, 0.45f, 1}, 0.0f, 0.3f);
    s.world().add<OscillatorComponent>(bob) = {{0, 0.4f, 0}, 0.4f, 0.0f, {}, false};
    s.world().add<RotatorComponent>(bob) = {{1, 1, 0}, 60.0f};

    const Entity pillar = add(s, "Metal Pillar", {2.4f, 1.0f, -0.4f});
    s.world().get<Transform>(pillar)->scale = {1.0f, 2.0f, 1.0f};
    mesh(s, pillar, "cylinder", {0.95f, 0.93f, 0.88f, 1}, 1.0f, 0.3f);

    const Entity lamp = add(s, "Warm Lamp", {0.5f, 2.2f, 2.5f});
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

void build_2d(Scene& s) {
    const Entity cam = add(s, "Main Camera", {0, 0, 10});
    auto& c = s.world().add<CameraComponent>(cam);
    c.projection = CameraComponent::Projection::Orthographic;
    c.ortho_height = 10.0f;

    sprite(s, add(s, "Sky", {0, 1, -1}), {40, 14}, {0.38f, 0.6f, 0.9f, 1}, -10);
    sprite(s, add(s, "Sun Disc", {5.5f, 3.2f, -0.5f}), {1.6f, 1.6f}, {1.0f, 0.86f, 0.35f, 1}, -9);
    sprite(s, add(s, "Ground", {0, -4.2f, 0}), {40, 2.4f}, {0.22f, 0.55f, 0.27f, 1}, 0);
    sprite(s, add(s, "Platform", {2.5f, -1.0f, 0}), {3.0f, 0.4f}, {0.45f, 0.3f, 0.2f, 1}, 0);
    sprite(s, add(s, "Platform High", {-1.5f, 1.0f, 0}), {2.5f, 0.4f}, {0.45f, 0.3f, 0.2f, 1}, 0);
    // Walls keep the player inside the camera view (half width ~8.9 at 16:9).
    sprite(s, add(s, "Platform Wall Left", {-9.3f, 1.0f, 0}), {1.0f, 14.0f}, {0.3f, 0.22f, 0.16f, 1}, 0);
    sprite(s, add(s, "Platform Wall Right", {9.3f, 1.0f, 0}), {1.0f, 14.0f}, {0.3f, 0.22f, 0.16f, 1}, 0);

    const Entity player = add(s, "Player", {-4.0f, -2.55f, 0});
    sprite(s, player, {0.9f, 0.9f}, {0.95f, 0.45f, 0.15f, 1}, 2);
    attach(s, player, "scripts/platformer.luau");

    const math::Vec3 coins[] = {{2.5f, -0.2f, 0}, {-1.5f, 1.8f, 0}, {6.0f, -2.4f, 0}};
    int n = 0;
    for (const math::Vec3& at : coins) {
        const Entity coin = add(s, "Coin " + std::to_string(++n), at);
        sprite(s, coin, {0.5f, 0.5f}, {1.0f, 0.78f, 0.12f, 1}, 1);
        attach(s, coin, "scripts/coin.luau");
    }

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

constexpr const char* kPlayer3d = R"lua(-- Drives the entity across the ground with WASD or the arrow keys and bobs it gently.
-- Fields below are defaults; override them per entity in the editor inspector.
local Player = {}
Player.speed = 5
Player.bounds = 9 -- stay within this distance of the origin on X and Z
Player.bob_height = 0.25

function Player:start()
    self.base_y = self.entity.position.y
    self.t = 0
end

function Player:update(dt)
    self.t += dt
    local x = input.axis("A", "D", "Left", "Right")
    local z = input.axis("W", "S", "Up", "Down")
    local p = self.entity.position
    local move = vector.create(x, 0, z)
    if vector.magnitude(move) > 0 then
        move = vector.normalize(move) * self.speed * dt
        -- roll in the direction of travel
        self.entity:rotate(vector.create(z, 0, -x), vector.magnitude(move) * 60)
    end
    local nx = math.clamp(p.x + move.x, -self.bounds, self.bounds)
    local nz = math.clamp(p.z + move.z, -self.bounds, self.bounds)
    self.entity.position = vector.create(nx, self.base_y + math.sin(self.t * 2) * self.bob_height, nz)
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

constexpr const char* kPlatformer = R"lua(-- Platformer controller: A/D or Left/Right to run, Space, W or Up to jump.
-- Collides with sprites named "Ground" or starting with `solid_prefix`, and collects
-- entities whose name starts with "Coin".
local Player = {}
Player.speed = 6
Player.jump_speed = 12
Player.gravity = 26
Player.solid_prefix = "Platform"

local function starts_with(s, prefix)
    return string.sub(s, 1, #prefix) == prefix
end

local function half_size(e)
    local size = e:get("Sprite").size
    local scale = e.scale
    return size[1] * 0.5 * scale.x, size[2] * 0.5 * scale.y
end

function Player:start()
    self.velocity = vector.zero
    self.grounded = false
    self.spawn = self.entity.position
    self.hx, self.hy = half_size(self.entity)
    self.solids = {}
    self.coins = {}
    for _, e in scene.query("Sprite") do
        local name = e.name
        if e ~= self.entity then
            if name == "Ground" or starts_with(name, self.solid_prefix) then
                local hx, hy = half_size(e)
                local p = e.position
                table.insert(self.solids, { x = p.x, y = p.y, hx = hx, hy = hy })
            elseif starts_with(name, "Coin") then
                table.insert(self.coins, e)
            end
        end
    end
    self.score = 0
    self.total = #self.coins
end

function Player:overlaps(x, y, s)
    return math.abs(x - s.x) < self.hx + s.hx - 1e-4 and math.abs(y - s.y) < self.hy + s.hy - 1e-4
end

function Player:fixed_update(dt)
    local v = self.velocity
    local vx = input.axis("A", "D", "Left", "Right") * self.speed
    local vy = v.y - self.gravity * dt
    if self.grounded and (input.down("Space") or input.down("W") or input.down("Up")) then
        vy = self.jump_speed
    end
    local p = self.entity.position
    local x = p.x + vx * dt
    for _, s in self.solids do
        if self:overlaps(x, p.y, s) then
            x = if vx > 0 then s.x - s.hx - self.hx else s.x + s.hx + self.hx
        end
    end
    local y = p.y + vy * dt
    self.grounded = false
    for _, s in self.solids do
        if self:overlaps(x, y, s) then
            if vy <= 0 then
                y = s.y + s.hy + self.hy
                self.grounded = true
            else
                y = s.y - s.hy - self.hy
            end
            vy = 0
        end
    end
    if y < -20 then -- fell off the world
        x, y, vy = self.spawn.x, self.spawn.y, 0
    end
    self.velocity = vector.create(vx, vy, 0)
    self.entity.position = vector.create(x, y, p.z)
end

function Player:update(dt)
    local p = self.entity.position
    for i = #self.coins, 1, -1 do
        local coin = self.coins[i]
        if not coin.valid then
            table.remove(self.coins, i)
        elseif vector.magnitude(coin.position - p) < 0.7 then
            coin:destroy()
            table.remove(self.coins, i)
            self.score += 1
            log.info("Coin collected:", self.score, "/", self.total)
            if self.score == self.total then log.info("All coins collected!") end
        end
    end
end

return Player
)lua";

constexpr const char* kCoin = R"lua(-- Spins and hovers; the player script collects it.
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
        {"3d", "3D Scene", "A ball you drive with WASD/arrow keys (Luau script), a follow camera, sun shadows, PBR primitives and a point light."},
        {"2d", "2D Game", "A playable platformer: run with A/D or arrows, jump with Space, collect the coins. Gameplay is in Luau scripts."},
        {"simulation", "Simulation",
         "576 entities animated by native systems, with a scripted orbiting camera: a starting point for simulation work."},
    };
    return list;
}

std::vector<TemplateFile> template_files(std::string_view id) {
    if (id == "3d")
        return {{"assets/scripts/player.luau", kPlayer3d}, {"assets/scripts/follow_camera.luau", kFollowCamera}};
    if (id == "2d") return {{"assets/scripts/platformer.luau", kPlatformer}, {"assets/scripts/coin.luau", kCoin}};
    if (id == "simulation") return {{"assets/scripts/orbit_camera.luau", kOrbitCamera}};
    return {};
}

void register_components() {
    gameplay::register_components();
    script::register_components();
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
