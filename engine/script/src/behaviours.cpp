// SPDX-License-Identifier: Apache-2.0
// Behaviours (ADR-0120): no-code gameplay components run by the script host. See behaviours.hpp.
//
// The ECS stores components in archetype tables, so adding or removing a component (or creating
// or destroying an entity) can move other components in memory. Every pass below first collects
// the entities it works on and re-reads component pointers after any structural change.
#include "script_internal.hpp"

#include "victus/animation/animation.hpp"
#include "victus/gameplay/framework.hpp"
#include "victus/scene/prefab.hpp"
#include "victus/scene/reflect.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>

namespace victus::script {

using math::Vec2;
using math::Vec3;
using scene::Scene;
using scene::Transform;

// ---- registration -------------------------------------------------------------------------------

const std::vector<std::string>& behaviour_names() {
    static const std::vector<std::string> names{"PlatformerMovement", "TopDownMovement", "CameraFollow", "Health",
                                                "Hazard", "Collectible", "Spawner", "Patrol", "MovingPlatform", "Chase", "Projectile",
                                                "Shooter", "Lifetime", "Checkpoint", "Goal", "Trigger", "JumpPad", "GameRules"};
    return names;
}

void register_behaviour_components() {
    static std::once_flag once;
    std::call_once(once, [] {
        const char* cat = "Behaviours";
        using PM = PlatformerMovementComponent;
        scene::Schema<PM>("PlatformerMovement", cat)
            .field(&PM::speed, "speed", {.min = 0, .max = 100, .speed = 0.1f, .tooltip = "Running speed (m/s)"})
            .field(&PM::acceleration, "acceleration", {.min = 0, .max = 1000, .speed = 1, .tooltip = "How quickly it reaches full speed"})
            .field(&PM::deceleration, "deceleration", {.min = 0, .max = 1000, .speed = 1, .tooltip = "How quickly it stops when you let go"})
            .field(&PM::jump_speed, "jump_speed", {.min = 0, .max = 100, .speed = 0.1f, .tooltip = "Higher jumps higher"})
            .field(&PM::max_jumps, "max_jumps", {.min = 0, .max = 10, .tooltip = "1 = normal jump, 2 = double jump, 0 = no jumping"})
            .field(&PM::coyote_time, "coyote_time", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "Seconds you can still jump after running off a ledge"})
            .field(&PM::jump_buffer, "jump_buffer", {.min = 0, .max = 1, .speed = 0.01f, .tooltip = "Seconds a jump press is remembered before landing"})
            .field(&PM::variable_jump, "variable_jump", {.tooltip = "Letting go of jump early makes a shorter jump"})
            .field(&PM::flip_sprite, "flip_sprite", {.tooltip = "Mirror the sprite to face the walking direction"})
            .field(&PM::depth_movement, "depth_movement", {.tooltip = "3D: up/down also walks forwards and back"})
            .field(&PM::move_action, "move_action", {.tooltip = "Input action for left/right (Project Settings > Input)"})
            .field(&PM::jump_action, "jump_action")
            .field(&PM::down_action, "down_action", {.tooltip = "Hold down and jump to drop through one-way platforms"})
            .field(&PM::jump_sound, "jump_sound", {.tooltip = "Sound played on every jump", .hint = "audio"})
            .commit();
        using TD = TopDownMovementComponent;
        scene::Schema<TD>("TopDownMovement", cat)
            .field(&TD::speed, "speed", {.min = 0, .max = 100, .speed = 0.1f, .tooltip = "Walking speed (m/s)"})
            .field(&TD::acceleration, "acceleration", {.min = 0, .max = 1000, .speed = 1})
            .field(&TD::deceleration, "deceleration", {.min = 0, .max = 1000, .speed = 1})
            .field(&TD::jump_speed, "jump_speed", {.min = 0, .max = 100, .speed = 0.1f, .tooltip = "3D only; 0 = no jumping"})
            .field(&TD::camera_relative, "camera_relative", {.tooltip = "3D: up walks away from the camera"})
            .field(&TD::face_movement, "face_movement", {.tooltip = "Turn to face where it walks"})
            .field(&TD::move_x_action, "move_x_action")
            .field(&TD::move_y_action, "move_y_action")
            .field(&TD::jump_action, "jump_action")
            .commit();
        using CF = CameraFollowComponent;
        scene::Schema<CF>("CameraFollow", cat)
            .field(&CF::target, "target", {.tooltip = "Name of the entity to follow; empty = the player"})
            .field(&CF::offset, "offset", {.speed = 0.05f, .tooltip = "Where to stay, relative to the target"})
            .field(&CF::smoothing, "smoothing", {.min = 0, .max = 100, .speed = 0.1f, .tooltip = "Higher is snappier; 0 locks on"})
            .field(&CF::look_at, "look_at", {.tooltip = "3D cameras turn to look at the target"})
            .field(&CF::follow_x, "follow_x")
            .field(&CF::follow_y, "follow_y")
            .field(&CF::follow_z, "follow_z")
            .commit();
        using HP = HealthComponent;
        scene::Schema<HP>("Health", cat)
            .field(&HP::max, "max", {.min = 0.001f, .max = 1e6f, .speed = 0.1f, .tooltip = "Full health"})
            .field(&HP::current, "current", {.min = 0, .max = 1e6f, .speed = 0.1f, .tooltip = "Health at the start"})
            .field(&HP::invulnerable_time, "invulnerable_time", {.min = 0, .max = 60, .speed = 0.05f, .tooltip = "Seconds of safety after being hit"})
            .enumeration(&HP::on_death, "on_death", {"destroy", "respawn", "restart_scene", "nothing"},
                         {.tooltip = "What happens at zero health"})
            .field(&HP::blink, "blink", {.tooltip = "Flash while invulnerable"})
            .commit();
        using HZ = HazardComponent;
        scene::Schema<HZ>("Hazard", cat)
            .field(&HZ::damage, "damage", {.min = 0, .max = 1e6f, .speed = 0.1f})
            .field(&HZ::target_tag, "target_tag", {.tooltip = "Who it hurts; Player also matches movement behaviours"})
            .field(&HZ::knockback, "knockback", {.min = 0, .max = 100, .speed = 0.1f, .tooltip = "Push speed away from the hazard"})
            .field(&HZ::stompable, "stompable", {.tooltip = "Landing on top defeats it (enemies)"})
            .field(&HZ::stomp_bounce, "stomp_bounce", {.min = 0, .max = 100, .speed = 0.1f})
            .field(&HZ::destroy_on_touch, "destroy_on_touch", {.tooltip = "Disappear after hurting (falling rocks)"})
            .field(&HZ::instant_kill, "instant_kill", {.tooltip = "Pits and lava: takes all health, even right after a hit"})
            .commit();
        using CO = CollectibleComponent;
        scene::Schema<CO>("Collectible", cat)
            .enumeration(&CO::kind, "kind", {"score", "health", "item"}, {.tooltip = "What picking it up gives"})
            .field(&CO::amount, "amount", {.min = 0, .max = 1e6f, .speed = 0.1f})
            .field(&CO::item, "item", {.tooltip = "Item kind: name added to the Inventory"})
            .field(&CO::collector_tag, "collector_tag")
            .field(&CO::sound, "sound", {.tooltip = "Sound played when picked up", .hint = "audio"})
            .field(&CO::message, "message", {.tooltip = "Shown on screen when picked up (items say \"You found a ...\" when empty)"})
            .commit();
        using SP = SpawnerComponent;
        scene::Schema<SP>("Spawner", cat)
            .field(&SP::prefab, "prefab", {.tooltip = "The .vprefab to create", .hint = "prefab"})
            .field(&SP::interval, "interval", {.min = 0.01f, .max = 3600, .speed = 0.05f, .tooltip = "Seconds between spawns"})
            .field(&SP::max_alive, "max_alive", {.min = 1, .max = 10000, .tooltip = "Stops while this many are alive"})
            .field(&SP::radius, "radius", {.min = 0, .max = 1000, .speed = 0.05f, .tooltip = "Random distance from the spawner"})
            .field(&SP::spawn_on_start, "spawn_on_start")
            .commit();
        using PA = PatrolComponent;
        scene::Schema<PA>("Patrol", cat)
            .field(&PA::offset, "offset", {.speed = 0.05f, .tooltip = "How far it walks from where it starts"})
            .field(&PA::speed, "speed", {.min = 0, .max = 100, .speed = 0.05f})
            .field(&PA::wait, "wait", {.min = 0, .max = 60, .speed = 0.05f, .tooltip = "Pause at each end (s)"})
            .field(&PA::flip_sprite, "flip_sprite")
            .field(&PA::turn_at_walls, "turn_at_walls", {.tooltip = "Turn around when a wall or other solid object is in the way"})
            .field(&PA::turn_at_ledges, "turn_at_ledges", {.tooltip = "Turn around instead of walking off the edge of a platform"})
            .commit();
        using MP = MovingPlatformComponent;
        scene::Schema<MP>("MovingPlatform", cat)
            .field(&MP::offset, "offset", {.speed = 0.05f, .tooltip = "Where it travels to, from where it starts"})
            .field(&MP::speed, "speed", {.min = 0, .max = 100, .speed = 0.05f, .tooltip = "Average speed (m/s)"})
            .field(&MP::wait, "wait", {.min = 0, .max = 60, .speed = 0.05f, .tooltip = "Pause at each end (s)"})
            .field(&MP::smooth, "smooth", {.tooltip = "Slow down and speed up gently at the ends"})
            .field(&MP::once, "once", {.tooltip = "Stop at the far end instead of coming back"})
            .field(&MP::wait_for_player, "wait_for_player", {.tooltip = "Start moving when the player first stands on it"})
            .field(&MP::wait_for_trigger, "wait_for_trigger", {.tooltip = "Stay put until a Trigger (a switch) starts it"})
            .field(&MP::spin, "spin", {.min = -3600, .max = 3600, .speed = 1, .tooltip = "Turning speed in degrees per second (saws, fans)"})
            .field(&MP::player_tag, "player_tag")
            .commit();
        using CH = ChaseComponent;
        scene::Schema<CH>("Chase", cat)
            .field(&CH::target_tag, "target_tag")
            .field(&CH::detect_radius, "detect_radius", {.min = 0, .max = 1000, .speed = 0.1f, .tooltip = "Starts chasing inside this distance"})
            .field(&CH::lose_radius, "lose_radius", {.min = 0, .max = 1000, .speed = 0.1f, .tooltip = "Gives up beyond this distance"})
            .field(&CH::speed, "speed", {.min = 0, .max = 100, .speed = 0.05f})
            .field(&CH::stop_distance, "stop_distance", {.min = 0, .max = 100, .speed = 0.05f})
            .commit();
        using PR = ProjectileComponent;
        scene::Schema<PR>("Projectile", cat)
            .field(&PR::velocity, "velocity", {.speed = 0.1f, .tooltip = "m/s"})
            .field(&PR::lifetime, "lifetime", {.min = 0.01f, .max = 3600, .speed = 0.05f})
            .field(&PR::damage, "damage", {.min = 0, .max = 1e6f, .speed = 0.1f})
            .field(&PR::target_tag, "target_tag", {.tooltip = "Enemy also matches anything with a Hazard"})
            .field(&PR::destroy_on_hit, "destroy_on_hit")
            .field(&PR::stop_at_walls, "stop_at_walls", {.tooltip = "Disappear when it hits a wall or other solid object"})
            .commit();
        using SH = ShooterComponent;
        scene::Schema<SH>("Shooter", cat)
            .field(&SH::prefab, "prefab", {.tooltip = "Projectile prefab; empty = a small bullet", .hint = "prefab"})
            .field(&SH::action, "action", {.tooltip = "Input action that fires"})
            .field(&SH::automatic, "automatic", {.tooltip = "Fire on its own at the nearest target (turrets)"})
            .field(&SH::cooldown, "cooldown", {.min = 0, .max = 60, .speed = 0.01f, .tooltip = "Seconds between shots"})
            .field(&SH::speed, "speed", {.min = 0, .max = 1000, .speed = 0.1f})
            .field(&SH::damage, "damage", {.min = 0, .max = 1e6f, .speed = 0.1f})
            .field(&SH::target_tag, "target_tag")
            .field(&SH::direction, "direction", {.speed = 0.05f, .tooltip = "Used when nothing else says where it faces"})
            .field(&SH::spawn_distance, "spawn_distance", {.min = 0, .max = 100, .speed = 0.05f})
            .commit();
        scene::Schema<LifetimeComponent>("Lifetime", cat)
            .field(&LifetimeComponent::seconds, "seconds", {.min = 0, .max = 3600, .speed = 0.05f})
            .commit();
        scene::Schema<CheckpointComponent>("Checkpoint", cat)
            .field(&CheckpointComponent::player_tag, "player_tag")
            .commit();
        using GO = GoalComponent;
        scene::Schema<GO>("Goal", cat)
            .enumeration(&GO::action, "action", {"win", "next_scene", "load_scene"})
            .field(&GO::scene, "scene", {.tooltip = "Scene to load, e.g. scenes/level2.vscene", .hint = "scene"})
            .field(&GO::require_all_collectibles, "require_all_collectibles", {.tooltip = "Only opens once every coin is collected"})
            .field(&GO::player_tag, "player_tag")
            .commit();
        using JP = JumpPadComponent;
        scene::Schema<JP>("JumpPad", cat)
            .field(&JP::strength, "strength", {.min = 0, .max = 200, .speed = 0.1f, .tooltip = "Launch speed (m/s)"})
            .field(&JP::direction, "direction", {.speed = 0.01f, .tooltip = "Which way it launches; (0, 1, 0) is straight up"})
            .field(&JP::target_tag, "target_tag")
            .field(&JP::sound, "sound", {.tooltip = "Sound played on every bounce", .hint = "audio"})
            .commit();
        using TR = TriggerComponent;
        scene::Schema<TR>("Trigger", cat)
            .field(&TR::player_tag, "player_tag")
            .field(&TR::message, "message", {.tooltip = "Text shown at the bottom of the screen when the player touches it"})
            .field(&TR::message_time, "message_time", {.min = 0, .max = 600, .speed = 0.1f, .tooltip = "Seconds; 0 = while touching"})
            .field(&TR::required_item, "required_item", {.tooltip = "Item the player must carry, e.g. Key (from an Item collectible)"})
            .field(&TR::consume_item, "consume_item", {.tooltip = "Use the item up"})
            .field(&TR::locked_message, "locked_message", {.tooltip = "Shown without the item; empty = \"You need a ...\""})
            .enumeration(&TR::action, "action", {"none", "start", "remove", "win"})
            .field(&TR::target, "target", {.tooltip = "Name of the object to start or remove; empty = this object"})
            .field(&TR::sound, "sound", {.tooltip = "Audio clip played when it fires", .hint = "audio"})
            .field(&TR::once, "once", {.tooltip = "Only the first time"})
            .commit();
        using GR = GameRulesComponent;
        scene::Schema<GR>("GameRules", cat)
            .field(&GR::score_to_win, "score_to_win", {.min = 0, .max = 1e9f, .speed = 1, .tooltip = "0 = no score goal"})
            .field(&GR::lives, "lives", {.min = 0, .max = 1000, .tooltip = "0 = unlimited"})
            .field(&GR::time_limit, "time_limit", {.min = 0, .max = 86400, .speed = 1, .tooltip = "Seconds; 0 = no limit"})
            .field(&GR::show_hud, "show_hud", {.tooltip = "Show score, lives and time on screen"})
            .field(&GR::restart_on_game_over, "restart_on_game_over")
            .field(&GR::restart_delay, "restart_delay", {.min = 0, .max = 60, .speed = 0.05f})
            .field(&GR::win_message, "win_message")
            .field(&GR::lose_message, "lose_message")
            .field(&GR::carry_over, "carry_over",
                   {.tooltip = "Keep score, lives and time when a Goal loads the next level"})
            .commit();
    });
}

namespace detail {

namespace {

constexpr const char* kHudName = "HUD (GameRules)";

struct Box {
    Vec3 c;
    Vec3 h;
    bool flat{false}; // 2D: compare X and Y only
};

Vec3 abs3(Vec3 v) { return {std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)}; }

Vec3 world_pos(Scene& s, Entity e) {
    if (s.parent(e).is_null()) {
        if (const auto* t = s.world().get<Transform>(e)) return t->position;
    }
    if (const auto* w = s.world().get<scene::WorldTransform>(e)) return w->matrix[3].xyz();
    if (const auto* t = s.world().get<Transform>(e)) return t->position;
    return {};
}

// Moves `e` so its world position becomes `p` (root entities and children of unrotated parents).
void set_world_pos(Scene& s, Entity e, Vec3 p) {
    auto* t = s.world().get<Transform>(e);
    if (!t) return;
    const Vec3 delta = p - world_pos(s, e);
    t->position += delta;
}

bool is_2d(Scene& s, Entity e) {
    auto& w = s.world();
    return w.has<scene::SpriteComponent>(e) || w.has<physics::Collider2DComponent>(e) ||
           w.has<physics::CharacterController2DComponent>(e) || w.has<physics::RigidBody2DComponent>(e);
}

Box bounds_of(Scene& s, Entity e) {
    auto& w = s.world();
    Box b;
    const Vec3 p = world_pos(s, e);
    const auto* t = w.get<Transform>(e);
    const Vec3 sc = t ? abs3(t->scale) : Vec3{1, 1, 1};
    b.c = p;
    if (const auto* c = w.get<physics::CharacterController2DComponent>(e)) {
        b.h = {c->size.x * 0.5f * sc.x, c->size.y * 0.5f * sc.y, 0.5f};
        b.flat = true;
    } else if (const auto* c2 = w.get<physics::Collider2DComponent>(e)) {
        Vec2 half{c2->size.x * 0.5f, c2->size.y * 0.5f};
        Vec2 center = c2->offset;
        using S = physics::Collider2DComponent::Shape;
        if (c2->shape == S::Circle) half = {c2->radius, c2->radius};
        if ((c2->shape == S::Polygon || c2->shape == S::Chain) && !c2->points.empty()) {
            Vec2 lo = c2->points[0], hi = c2->points[0];
            for (const Vec2 q : c2->points) {
                lo = {std::min(lo.x, q.x), std::min(lo.y, q.y)};
                hi = {std::max(hi.x, q.x), std::max(hi.y, q.y)};
            }
            half = (hi - lo) * 0.5f;
            center = center + (lo + hi) * 0.5f;
        }
        b.c = p + Vec3{center.x * sc.x, center.y * sc.y, 0};
        b.h = {half.x * sc.x, half.y * sc.y, 0.5f};
        b.flat = true;
    } else if (const auto* cc = w.get<physics::CharacterControllerComponent>(e)) {
        b.c = p + Vec3{0, cc->height * 0.5f * sc.y, 0};
        b.h = {cc->radius * sc.x, cc->height * 0.5f * sc.y, cc->radius * sc.z};
    } else if (const auto* col = w.get<physics::ColliderComponent>(e)) {
        using S = physics::ColliderComponent::Shape;
        Vec3 half = col->size * 0.5f;
        if (col->shape == S::Sphere) half = Vec3{col->radius};
        else if (col->shape == S::Capsule || col->shape == S::Cylinder)
            half = {col->radius, std::max(col->height * 0.5f, col->radius), col->radius};
        else if (col->shape == S::Convex || col->shape == S::Mesh) half = Vec3{0.5f};
        b.c = p + col->offset * sc;
        b.h = half * sc;
    } else if (const auto* sp = w.get<scene::SpriteComponent>(e)) {
        const Vec2 size{sp->size.x * sc.x, sp->size.y * sc.y};
        b.c = p + Vec3{(0.5f - sp->pivot.x) * size.x, (0.5f - sp->pivot.y) * size.y, 0};
        b.h = {size.x * 0.5f, size.y * 0.5f, 0.5f};
        b.flat = true;
    } else {
        b.h = sc * 0.5f;
        b.flat = is_2d(s, e);
    }
    return b;
}

bool overlaps(const Box& a, const Box& b) {
    const bool flat = a.flat || b.flat;
    if (std::fabs(a.c.x - b.c.x) > a.h.x + b.h.x) return false;
    if (std::fabs(a.c.y - b.c.y) > a.h.y + b.h.y) return false;
    if (!flat && std::fabs(a.c.z - b.c.z) > a.h.z + b.h.z) return false;
    return true;
}

bool player_controlled(Scene& s, Entity e) {
    return s.world().has<PlatformerMovementComponent>(e) || s.world().has<TopDownMovementComponent>(e);
}

// Entities matching `tag`, cached for the pass.
struct TagCache {
    Scene& s;
    std::unordered_map<std::string, std::vector<Entity>> by_tag;
    const std::vector<Entity>& get(const std::string& tag) {
        auto it = by_tag.find(tag);
        if (it != by_tag.end()) return it->second;
        std::vector<Entity> out;
        if (!tag.empty()) out = gameplay::find_tagged(s, tag);
        auto add = [&](Entity e) {
            if (std::find(out.begin(), out.end(), e) == out.end()) out.push_back(e);
        };
        if (tag == "Player" || tag.empty()) {
            s.world().each<PlatformerMovementComponent>([&](Entity e, PlatformerMovementComponent&) { add(e); });
            s.world().each<TopDownMovementComponent>([&](Entity e, TopDownMovementComponent&) { add(e); });
        }
        if (tag == "Enemy" || tag.empty())
            s.world().each<HazardComponent>([&](Entity e, HazardComponent&) {
                if (!player_controlled(s, e)) add(e);
            });
        if (tag.empty()) s.world().each<HealthComponent>([&](Entity e, HealthComponent&) { add(e); });
        return by_tag.emplace(tag, std::move(out)).first->second;
    }
};

// Solid scenery for wall and ledge tests: colliders that block movement and belong to no actor
// (characters, pickups, hazards, projectiles and other behaviour-driven entities are skipped).
struct Solid {
    Entity e;
    Box box;
    bool one_way{false}; // stands on it, walks and shoots through it
};

bool is_actor(Scene& s, Entity e) {
    auto& w = s.world();
    return w.has<physics::CharacterController2DComponent>(e) || w.has<physics::CharacterControllerComponent>(e) ||
           w.has<HealthComponent>(e) || w.has<HazardComponent>(e) || w.has<CollectibleComponent>(e) ||
           w.has<ProjectileComponent>(e) || w.has<PatrolComponent>(e) || w.has<ChaseComponent>(e) ||
           w.has<CheckpointComponent>(e) || w.has<GoalComponent>(e) || player_controlled(s, e) ||
           (w.has<MovingPlatformComponent>(e) && w.has<HazardComponent>(e));
}

std::vector<Solid> gather_solids(Scene& s) {
    std::vector<Solid> out;
    s.world().each<physics::Collider2DComponent>([&](Entity e, physics::Collider2DComponent& c) {
        if (!c.is_trigger) out.push_back({e, {}, c.one_way});
    });
    s.world().each<physics::ColliderComponent>([&](Entity e, physics::ColliderComponent& c) {
        if (!c.is_trigger) out.push_back({e, {}, false});
    });
    std::erase_if(out, [&](const Solid& so) { return is_actor(s, so.e); });
    for (Solid& so : out) so.box = bounds_of(s, so.e);
    return out;
}

bool hits_solid(const std::vector<Solid>& solids, const Box& b, Entity self, bool include_one_way) {
    for (const Solid& so : solids)
        if (so.e != self && (include_one_way || !so.one_way) && overlaps(b, so.box)) return true;
    return false;
}

float approach(float v, float target, float step) {
    if (v < target) return std::min(v + step, target);
    return std::max(v - step, target);
}
Vec3 approach(Vec3 v, Vec3 target, float step) {
    const Vec3 d = target - v;
    const float len = math::length(d);
    if (len <= step || len <= 0) return target;
    return v + d * (step / len);
}

const gameplay::InputSource* input_for(ScriptHost::Impl& h, Entity e) {
    h.route(e);
    return h.input;
}
float action_value(ScriptHost::Impl& h, const gameplay::InputSource* in, std::string_view name) {
    const auto* a = h.options.input_map.find(name);
    return a ? gameplay::InputMap::value(in, *a) : 0.0f;
}
bool action_pressed(ScriptHost::Impl& h, const gameplay::InputSource* in, std::string_view name) {
    const auto* a = h.options.input_map.find(name);
    return a && gameplay::InputMap::pressed(in, *a);
}
bool action_released(ScriptHost::Impl& h, const gameplay::InputSource* in, std::string_view name) {
    const auto* a = h.options.input_map.find(name);
    return a && gameplay::InputMap::released(in, *a);
}

float random01(ScriptHost::Impl& h) {
    // SplitMix64: deterministic so captures and replays repeat spawns.
    std::uint64_t z = (h.behaviours.rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<float>(z >> 40) / static_cast<float>(1ull << 24);
}

void warn_once(ScriptHost::Impl& h, Entity e, std::string message) {
    if (h.behaviours.warned.emplace(e.bits(), true).second) h.report(std::move(message));
}

std::string name_of(Scene& s, Entity e) {
    if (const auto* n = s.world().get<scene::Name>(e)) return n->value;
    return "entity";
}

bool has_character(physics::PhysicsWorld* pw, Entity e) { return pw && pw->character_state(e).valid; }

Entity rules_entity(Scene& s) {
    Entity found;
    s.world().each<GameRulesComponent>([&](Entity e, GameRulesComponent&) {
        if (found.is_null()) found = e;
    });
    return found;
}

std::string format_time(float seconds) {
    const int t = static_cast<int>(std::ceil(std::max(0.0f, seconds)));
    return std::format("{}:{:02}", t / 60, t % 60);
}
std::string format_number(float v) {
    if (std::fabs(v - std::round(v)) < 1e-4f) return std::format("{}", static_cast<long long>(std::llround(v)));
    return std::format("{:.1f}", v);
}

// ---- game flow ----------------------------------------------------------------------------------

void ensure_start_snapshot(ScriptHost::Impl& h, Scene& s) {
    if (h.behaviours.has_start) return;
    h.behaviours.has_start = true;
    h.behaviours.start_doc = s.save();
}

void request_restart(ScriptHost::Impl& h) {
    if (h.pending_scene || !h.behaviours.has_start) return;
    h.pending_restore.reset();
    h.pending_scene = ScriptHost::Impl::SceneRequest{h.current_scene, h.behaviours.start_doc, h.scene_params};
    h.behaviours.carry = h.behaviours.entry;
    if (h.behaviours.carry) h.behaviours.carry->lives_left = 0; // a fresh set of lives
}

void request_scene(ScriptHost::Impl& h, Scene& s, Entity from, const std::string& path) {
    auto doc = read_scene(h, path);
    if (!doc) {
        warn_once(h, from, std::format("Goal: cannot load scene '{}': {}", path, doc.error().message));
        return;
    }
    h.pending_restore.reset();
    h.pending_scene = ScriptHost::Impl::SceneRequest{path, std::move(*doc), Value::object()};
    h.behaviours.carry.reset();
    if (const Entity r = rules_entity(s); !r.is_null()) {
        const auto* g = s.world().get<GameRulesComponent>(r);
        if (g->carry_over && g->initialised) h.behaviours.carry = ScriptHost::Impl::Behaviours::Carry{g->score, g->lives_left, g->elapsed};
    }
}

void end_game(Scene& s, Entity rules, bool won) {
    auto* r = s.world().get<GameRulesComponent>(rules);
    if (!r || r->state != GameState::Playing) return;
    r->state = won ? GameState::Won : GameState::Lost;
    r->end_timer = 0;
    Value p = Value::object();
    p.set("score", r->score);
    gameplay::emit_event(s, won ? "game_won" : "game_lost", std::move(p), rules);
}

bool playing(Scene& s) {
    const Entity r = rules_entity(s);
    if (r.is_null()) return true;
    const auto* g = s.world().get<GameRulesComponent>(r);
    return !g || g->state == GameState::Playing;
}

void add_score(Scene& s, float amount) {
    const Entity r = rules_entity(s);
    auto* g = r.is_null() ? nullptr : s.world().get<GameRulesComponent>(r);
    if (!g) return;
    g->score += amount;
    Value p = Value::object();
    p.set("score", g->score);
    gameplay::emit_event(s, "score_changed", std::move(p), r);
}

void respawn(Scene& s, physics::PhysicsWorld* pw, Entity e) {
    auto* hp = s.world().get<HealthComponent>(e);
    if (!hp) return;
    hp->current = hp->max;
    hp->dead = false;
    hp->invulnerable = hp->invulnerable_time;
    const Vec3 at = hp->respawn_point;
    set_world_pos(s, e, at);
    if (pw && pw->has_body(e)) pw->set_linear_velocity(e, {});
    if (has_character(pw, e)) pw->move_character(e, {});
    if (auto* pm = s.world().get<PlatformerMovementComponent>(e)) {
        pm->velocity_x = 0;
        pm->jumps_used = 0;
    }
    if (auto* td = s.world().get<TopDownMovementComponent>(e)) td->velocity = {};
}

void die(ScriptHost::Impl& h, Scene& s, physics::PhysicsWorld* pw, Entity e, std::vector<Entity>& doomed) {
    auto* hp = s.world().get<HealthComponent>(e);
    if (!hp || hp->dead) return;
    hp->dead = true;
    const DeathAction action = hp->on_death;
    gameplay::emit_event(s, "died", Value::object(), e, e);
    const bool player = player_controlled(s, e) || gameplay::has_tag(s, e, "Player");
    if (player) {
        const Entity r = rules_entity(s);
        if (auto* g = r.is_null() ? nullptr : s.world().get<GameRulesComponent>(r); g && g->lives > 0) {
            g->lives_left = g->lives_left > 0 ? g->lives_left - 1 : 0;
            Value p = Value::object();
            p.set("lives", g->lives_left);
            gameplay::emit_event(s, "life_lost", std::move(p), r);
            if (g->lives_left == 0) {
                end_game(s, r, false);
                if (action == DeathAction::Destroy) doomed.push_back(e);
                return;
            }
        }
    }
    switch (action) {
        case DeathAction::Destroy: doomed.push_back(e); break;
        case DeathAction::Respawn: respawn(s, pw, e); break;
        case DeathAction::RestartScene: request_restart(h); break;
        case DeathAction::Nothing: break;
    }
}

// Returns true when the hit landed (not invulnerable, not already dead).
bool damage(ScriptHost::Impl& h, Scene& s, physics::PhysicsWorld* pw, Entity target, float amount, Entity source,
            std::vector<Entity>& doomed) {
    auto* hp = s.world().get<HealthComponent>(target);
    if (!hp || hp->dead || hp->invulnerable > 0 || amount <= 0) return false;
    hp->current = std::max(0.0f, hp->current - amount);
    hp->invulnerable = hp->invulnerable_time;
    const float now = hp->current;
    Value p = Value::object();
    p.set("amount", amount);
    p.set("health", now);
    gameplay::emit_event(s, "damaged", std::move(p), source, target);
    if (now <= 0) die(h, s, pw, target, doomed);
    return true;
}

// ---- movement (fixed step) ------------------------------------------------------------------------

void ensure_controllers(Scene& s) {
    std::vector<std::pair<Entity, bool>> need; // entity, top-down
    auto& w = s.world();
    w.each<PlatformerMovementComponent>([&](Entity e, PlatformerMovementComponent&) {
        if (!w.has<physics::CharacterController2DComponent>(e) && !w.has<physics::CharacterControllerComponent>(e))
            need.emplace_back(e, false);
    });
    w.each<TopDownMovementComponent>([&](Entity e, TopDownMovementComponent&) {
        if (!w.has<physics::CharacterController2DComponent>(e) && !w.has<physics::CharacterControllerComponent>(e))
            need.emplace_back(e, true);
    });
    for (const auto& [e, top_down] : need) {
        if (is_2d(s, e)) {
            physics::CharacterController2DComponent c;
            if (const auto* sp = w.get<scene::SpriteComponent>(e)) c.size = {sp->size.x * 0.8f, sp->size.y * 0.95f};
            c.top_down = top_down;
            w.add<physics::CharacterController2DComponent>(e, c);
        } else {
            w.add<physics::CharacterControllerComponent>(e);
        }
    }
    std::vector<Entity> flat;
    w.each<TopDownMovementComponent, physics::CharacterController2DComponent>(
        [&](Entity e, TopDownMovementComponent&, physics::CharacterController2DComponent& c) {
            if (!c.top_down) flat.push_back(e);
        });
    for (const Entity e : flat) w.get<physics::CharacterController2DComponent>(e)->top_down = true;
}

// Animated characters: movement behaviours feed the first Animator on the entity or below it
// with the parameters the starter characters' controller uses: "speed" (m/s along the ground),
// "grounded" (true until 0.15 s in the air, so step edges are not falls) and a "jump" trigger.
// Controllers without those parameters ignore them.
Entity animator_of(Scene& s, Entity e) {
    std::vector<Entity> layer{e};
    for (int depth = 0; depth < 5 && !layer.empty(); ++depth) {
        std::vector<Entity> next;
        for (const Entity x : layer) {
            if (s.world().has<animation::AnimatorComponent>(x)) return x;
            for (const Entity c : s.children(x)) next.push_back(c);
        }
        layer = std::move(next);
    }
    return {};
}

void animate(Scene& s, Entity e, float speed, float air_time, bool jumped) {
    auto* sys = animation::system_of(s);
    if (!sys) return;
    const Entity a = animator_of(s, e);
    if (a.is_null()) return;
    (void)sys->set_parameter(s, a, "speed", speed);
    (void)sys->set_parameter(s, a, "grounded", air_time < 0.15f ? 1.0f : 0.0f);
    if (jumped) (void)sys->set_trigger(s, a, "jump");
}

void face_sprite(Scene& s, Entity e, float dir) {
    if (auto* sp = s.world().get<scene::SpriteComponent>(e); sp && dir != 0) sp->flip_x = dir < 0;
}

void platformer_step(ScriptHost::Impl& h, Scene& s, physics::PhysicsWorld* pw, Entity e, float dt, bool active) {
    const gameplay::InputSource* in = active ? input_for(h, e) : nullptr;
    auto* m = s.world().get<PlatformerMovementComponent>(e);
    if (!m) return;
    const float axis = math::clamp(action_value(h, in, m->move_action), -1, 1);
    const float target = axis * m->speed;
    const bool turning = axis != 0 && m->velocity_x != 0 && (axis > 0) != (m->velocity_x > 0);
    const float rate = std::fabs(axis) > 0.01f ? m->acceleration + (turning ? m->deceleration : 0) : m->deceleration;
    m->velocity_x = approach(m->velocity_x, target, rate * dt);
    if (axis > 0.01f) m->facing = 1;
    else if (axis < -0.01f) m->facing = -1;
    const bool flat = s.world().has<physics::CharacterController2DComponent>(e);
    const float depth = (!flat && m->depth_movement) ? -action_value(h, in, m->down_action) * m->speed : 0.0f;
    m->buffer = std::max(0.0f, m->buffer - dt);

    bool jumped = false;
    if (has_character(pw, e)) {
        const physics::CharacterState st = pw->character_state(e);
        m->air_time = st.grounded ? 0.0f : m->air_time + dt;
        if (st.grounded && st.velocity.y <= 0.01f) {
            m->coyote = m->coyote_time;
            m->jumps_used = 0;
            m->jump_cut_ready = false;
        } else {
            m->coyote = std::max(0.0f, m->coyote - dt);
            if (m->jumps_used == 0 && m->coyote <= 0) m->jumps_used = 1; // walked off: the ground jump is gone
        }
        Vec3 vel = st.velocity;
        if (m->buffer > 0 && flat && st.grounded && action_value(h, in, m->down_action) < -0.5f &&
            pw->drop_through(e, 0.3f)) {
            m->buffer = 0;
        }
        if (m->buffer > 0 && m->max_jumps > 0) {
            const bool ground_jump = m->jumps_used == 0 && (st.grounded || m->coyote > 0);
            if (ground_jump || m->jumps_used < m->max_jumps) {
                if (!(st.grounded && pw->jump_character(e, m->jump_speed))) {
                    vel.y = m->jump_speed;
                    pw->set_linear_velocity(e, vel);
                }
                m->jumps_used = ground_jump ? 1 : m->jumps_used + 1;
                jumped = true;
                if (!m->jump_sound.empty())
                    if (auto* a = audio::engine_of(s)) {
                        if (auto played = a->play_one_shot(m->jump_sound); !played)
                            warn_once(h, e, std::format("PlatformerMovement jump sound '{}': {}", m->jump_sound, played.error().message));
                        m = s.world().get<PlatformerMovementComponent>(e);
                    }
                m->coyote = 0;
                m->buffer = 0;
                m->jump_held = true;
                m->jump_cut_ready = m->variable_jump;
            }
        }
        if (m->jump_cut_ready && !m->jump_held) {
            m->jump_cut_ready = false;
            if (vel.y > 0) {
                vel.y *= 0.5f;
                pw->set_linear_velocity(e, vel);
            }
        }
        pw->move_character(e, {m->velocity_x, 0, depth});
    } else if (!pw) {
        // No physics in this run: slide the transform so the scene still responds to input.
        if (auto* t = s.world().get<Transform>(e)) t->position += Vec3{m->velocity_x * dt, 0, depth * dt};
    }
    const float ground_speed = std::sqrt(m->velocity_x * m->velocity_x + depth * depth);
    const float air = m->air_time;
    if (m->flip_sprite) face_sprite(s, e, m->facing);
    if (!flat) animate(s, e, ground_speed, air, jumped);
}

Vec3 camera_forward(Scene& s) {
    Vec3 f{0, 0, -1};
    bool found = false;
    s.world().each<scene::CameraComponent, Transform>([&](Entity, scene::CameraComponent& c, Transform& t) {
        if (found || !c.primary) return;
        found = true;
        f = t.rotation.rotate({0, 0, -1});
    });
    f.y = 0;
    const float len = math::length(f);
    return len > 1e-3f ? f / len : Vec3{0, 0, -1};
}

void top_down_step(ScriptHost::Impl& h, Scene& s, physics::PhysicsWorld* pw, Entity e, float dt, bool active) {
    const gameplay::InputSource* in = active ? input_for(h, e) : nullptr;
    const bool flat = is_2d(s, e);
    const Vec3 cam = flat ? Vec3{} : camera_forward(s);
    auto* m = s.world().get<TopDownMovementComponent>(e);
    if (!m) return;
    Vec2 stick{action_value(h, in, m->move_x_action), action_value(h, in, m->move_y_action)};
    if (math::length(stick) > 1) stick = math::normalize(stick);
    Vec3 dir;
    if (flat) {
        dir = {stick.x, stick.y, 0};
    } else if (m->camera_relative) {
        const Vec3 right{-cam.z, 0, cam.x};
        dir = right * stick.x + cam * stick.y;
    } else {
        dir = {stick.x, 0, -stick.y};
    }
    const bool moving = math::length(dir) > 0.01f;
    m->velocity = approach(m->velocity, dir * m->speed, (moving ? m->acceleration : m->deceleration) * dt);
    if (moving) m->facing = math::normalize(dir);
    const Vec3 vel = m->velocity;
    const Vec3 facing = m->facing;
    const bool face = m->face_movement;
    const float jump_speed = m->jump_speed;
    const bool jump = !flat && jump_speed > 0 && action_pressed(h, in, m->jump_action);
    bool jumped = false;
    float air = 0;
    if (has_character(pw, e)) {
        const bool grounded = pw->character_state(e).grounded;
        m->air_time = grounded ? 0.0f : m->air_time + dt;
        air = m->air_time;
        pw->move_character(e, vel);
        if (jump) jumped = pw->jump_character(e, jump_speed);
    } else if (!pw) {
        if (auto* t = s.world().get<Transform>(e)) t->position += vel * dt;
    }
    if (!flat) animate(s, e, math::length(Vec3{vel.x, 0, vel.z}), air, jumped);
    if (face && moving) {
        if (flat) {
            face_sprite(s, e, facing.x > 0.01f ? 1.0f : (facing.x < -0.01f ? -1.0f : 0.0f));
        } else if (auto* t = s.world().get<Transform>(e)) {
            t->rotation = math::Quat::axis_angle({0, 1, 0}, std::atan2(-facing.x, -facing.z));
        }
    }
}

// Moves a non-player entity with velocity `v` (a character walks; anything else slides).
void drive(Scene& s, physics::PhysicsWorld* pw, Entity e, Vec3 v, float dt) {
    if (has_character(pw, e)) {
        pw->move_character(e, v);
        if (!is_2d(s, e)) {
            const Vec3 flat_v{v.x, 0, v.z};
            const float speed = math::length(flat_v);
            animate(s, e, speed, 0, false);
            // 3D walkers face where they go (-Z forward, as Top-Down Movement turns the player).
            if (auto* t = s.world().get<Transform>(e); t && speed > 0.05f)
                t->rotation = math::Quat::axis_angle({0, 1, 0}, std::atan2(-v.x, -v.z));
        }
        return;
    }
    if (auto* t = s.world().get<Transform>(e)) t->position += v * dt;
}

// Direction a non-player mover may travel in: side-view 2D characters only walk along X,
// 3D characters along the ground.
Vec3 constrain(Scene& s, Entity e, Vec3 d) {
    if (const auto* c2 = s.world().get<physics::CharacterController2DComponent>(e)) {
        if (!c2->top_down) d = {d.x, 0, 0};
        else d.z = 0;
    } else if (s.world().has<physics::CharacterControllerComponent>(e)) {
        d.y = 0;
    } else if (is_2d(s, e)) {
        d.z = 0;
    }
    return d;
}

// True when a patrol moving with velocity `v` should turn: a solid just ahead, or no ground ahead.
bool blocked(Scene& s, Entity e, const PatrolComponent& pa, Vec3 v, const std::vector<Solid>& solids) {
    const Box b = bounds_of(s, e);
    const Vec3 dir = math::normalize(v);
    if (pa.turn_at_walls) {
        Box ahead = b;
        ahead.h = b.h * 0.9f; // clear of the floor it stands on and the ceiling above
        ahead.c = b.c + dir * (0.1f * std::max({b.h.x, b.h.y, b.h.z}) + 0.05f);
        if (hits_solid(solids, ahead, e, false)) return true;
    }
    const auto* c2 = s.world().get<physics::CharacterController2DComponent>(e);
    const bool side_view_2d = (c2 && !c2->top_down) || (!c2 && is_2d(s, e));
    const bool walker_3d = s.world().has<physics::CharacterControllerComponent>(e);
    if (pa.turn_at_ledges && (side_view_2d || walker_3d)) {
        Box foot;
        foot.flat = b.flat;
        foot.h = {0.05f, 0.15f, 0.05f};
        foot.c = b.c + Vec3{dir.x * (b.h.x + 0.05f), -b.h.y - 0.1f, walker_3d ? dir.z * (b.h.z + 0.05f) : 0};
        if (!hits_solid(solids, foot, e, true)) return true;
    }
    return false;
}

void ai_movement(Scene& s, physics::PhysicsWorld* pw, float dt, TagCache& tags, bool active) {
    std::vector<Entity> chasers, patrols;
    s.world().each<ChaseComponent>([&](Entity e, ChaseComponent&) { chasers.push_back(e); });
    s.world().each<PatrolComponent>([&](Entity e, PatrolComponent&) { patrols.push_back(e); });
    const std::vector<Solid> solids = patrols.empty() ? std::vector<Solid>{} : gather_solids(s);

    std::vector<Entity> chasing;
    for (const Entity e : chasers) {
        auto* c = s.world().get<ChaseComponent>(e);
        if (!c) continue;
        const Vec3 p = world_pos(s, e);
        Entity best;
        float best_d = 1e30f;
        for (const Entity t : tags.get(c->target_tag)) {
            if (t == e || !s.world().alive(t)) continue;
            if (const auto* hp = s.world().get<HealthComponent>(t); hp && hp->dead) continue;
            const float d = math::length(constrain(s, e, world_pos(s, t) - p));
            if (d < best_d) best_d = d, best = t;
        }
        c = s.world().get<ChaseComponent>(e);
        c->chasing = active && !best.is_null() && best_d <= (c->chasing ? c->lose_radius : c->detect_radius);
        if (!c->chasing) {
            if (!s.world().has<PatrolComponent>(e)) drive(s, pw, e, {}, dt);
            continue;
        }
        chasing.push_back(e);
        const Vec3 to = constrain(s, e, world_pos(s, best) - p);
        const float speed = c->speed;
        const Vec3 v = best_d > c->stop_distance ? math::normalize(to) * speed : Vec3{};
        if (v.x != 0) face_sprite(s, e, v.x);
        drive(s, pw, e, v, dt);
    }

    for (const Entity e : patrols) {
        if (std::find(chasing.begin(), chasing.end(), e) != chasing.end()) continue;
        auto* pa = s.world().get<PatrolComponent>(e);
        if (!pa) continue;
        const Vec3 p = world_pos(s, e);
        if (!pa->has_origin) {
            pa->origin = p;
            pa->has_origin = true;
        }
        if (!active) {
            drive(s, pw, e, {}, dt);
            continue;
        }
        const float len2 = math::dot(pa->offset, pa->offset);
        if (len2 < 1e-8f || pa->speed <= 0) continue;
        if (pa->waiting > 0) {
            pa->waiting -= dt;
            drive(s, pw, e, {}, dt);
            continue;
        }
        const float t = math::dot(p - pa->origin, pa->offset) / len2;
        if (!pa->returning && t >= 1) pa->returning = true, pa->waiting = pa->wait;
        else if (pa->returning && t <= 0) pa->returning = false, pa->waiting = pa->wait;
        if (pa->waiting > 0) {
            drive(s, pw, e, {}, dt);
            continue;
        }
        const Vec3 goal = pa->returning ? pa->origin : pa->origin + pa->offset;
        Vec3 to = goal - p;
        const float dist = math::length(to);
        const float step = pa->speed * dt;
        const bool flip = pa->flip_sprite;
        Vec3 v = dist > 1e-5f ? to / dist * pa->speed : Vec3{};
        if (!has_character(pw, e) && dist < step) v = to / std::max(dt, 1e-6f); // land exactly on the end
        v = constrain(s, e, v);
        if ((pa->turn_at_walls || pa->turn_at_ledges) && math::dot(v, v) > 1e-8f && blocked(s, e, *pa, v, solids)) {
            pa->returning = !pa->returning;
            pa->waiting = pa->wait;
            drive(s, pw, e, {}, dt);
            continue;
        }
        if (flip && v.x != 0) face_sprite(s, e, v.x);
        drive(s, pw, e, v, dt);
    }
}

// Moving platforms are kinematic bodies so the physics step gives them a velocity and characters
// standing on them are carried. A platform without a collider gets one sized from its sprite or mesh.
void ensure_platform_bodies(Scene& s) {
    std::vector<Entity> list;
    s.world().each<MovingPlatformComponent>([&](Entity e, MovingPlatformComponent&) { list.push_back(e); });
    auto& w = s.world();
    for (const Entity e : list) {
        if (is_2d(s, e)) {
            if (!w.has<physics::Collider2DComponent>(e) && !w.has<physics::ColliderComponent>(e)) {
                physics::Collider2DComponent c;
                if (const auto* sp = w.get<scene::SpriteComponent>(e)) c.size = {sp->size.x, sp->size.y};
                w.add<physics::Collider2DComponent>(e, c);
            }
            if (!w.has<physics::Collider2DComponent>(e)) continue;
            auto* rb = w.get<physics::RigidBody2DComponent>(e);
            if (!rb) {
                physics::RigidBody2DComponent r;
                r.type = physics::BodyType::Kinematic;
                w.add<physics::RigidBody2DComponent>(e, r);
            } else if (rb->type != physics::BodyType::Kinematic) {
                rb->type = physics::BodyType::Kinematic;
            }
        } else {
            if (!w.has<physics::ColliderComponent>(e)) {
                if (!w.has<scene::MeshRendererComponent>(e)) continue;
                w.add<physics::ColliderComponent>(e);
            }
            auto* rb = w.get<physics::RigidBodyComponent>(e);
            if (!rb) {
                physics::RigidBodyComponent r;
                r.type = physics::BodyType::Kinematic;
                w.add<physics::RigidBodyComponent>(e, r);
            } else if (rb->type != physics::BodyType::Kinematic) {
                rb->type = physics::BodyType::Kinematic;
            }
        }
    }
}

void platforms_move(Scene& s, float dt, TagCache& tags, bool active) {
    std::vector<Entity> list;
    s.world().each<MovingPlatformComponent>([&](Entity e, MovingPlatformComponent&) { list.push_back(e); });
    for (const Entity e : list) {
        auto* mp = s.world().get<MovingPlatformComponent>(e);
        auto* t = s.world().get<Transform>(e);
        if (!mp || !t) continue;
        if (!mp->has_origin) {
            mp->origin = t->position;
            mp->base_rotation = t->rotation;
            mp->has_origin = true;
        }
        if (!active) continue;
        if (mp->wait_for_trigger && !mp->triggered) continue;
        if (mp->wait_for_player && !mp->started) {
            Box top = bounds_of(s, e);
            top.c.y += 0.1f; // standing on it, not only touching a side
            top.h.y += 0.1f;
            for (const Entity p : tags.get(mp->player_tag))
                if (p != e && s.world().alive(p) && overlaps(top, bounds_of(s, p))) mp->started = true;
            if (!mp->started) continue;
        }
        mp->started = true;
        if (mp->spin != 0) {
            mp->angle = std::fmod(mp->angle + mp->spin * dt, 360.0f);
            const Vec3 axis = is_2d(s, e) ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
            t->rotation = mp->base_rotation * math::Quat::axis_angle(axis, math::radians(mp->angle));
        }
        const float len = math::length(mp->offset);
        if (len < 1e-5f || mp->speed <= 0) continue;
        if (mp->waiting > 0) {
            mp->waiting -= dt;
            continue;
        }
        const bool at_end = !mp->returning && mp->progress >= 1.0f;
        if (at_end && mp->once) continue;
        mp->progress += (mp->returning ? -1.0f : 1.0f) * mp->speed * dt / len;
        if (mp->progress >= 1.0f) {
            mp->progress = 1.0f;
            if (!mp->once) mp->returning = true;
            mp->waiting = mp->wait;
        } else if (mp->progress <= 0.0f) {
            mp->progress = 0.0f;
            mp->returning = false;
            mp->waiting = mp->wait;
        }
        const float u = mp->smooth ? mp->progress * mp->progress * (3.0f - 2.0f * mp->progress) : mp->progress;
        t->position = mp->origin + mp->offset * u;
    }
}

void projectiles_move(Scene& s, float dt) {
    s.world().each<ProjectileComponent, Transform>([&](ProjectileComponent& p, Transform& t) {
        t.position += p.velocity * dt;
    });
}

// ---- per-frame passes ----------------------------------------------------------------------------

void movement_input(ScriptHost::Impl& h, Scene& s) {
    std::vector<Entity> list;
    s.world().each<PlatformerMovementComponent>([&](Entity e, PlatformerMovementComponent&) { list.push_back(e); });
    for (const Entity e : list) {
        const gameplay::InputSource* in = input_for(h, e);
        auto* m = s.world().get<PlatformerMovementComponent>(e);
        if (action_pressed(h, in, m->jump_action)) m->buffer = std::max(m->jump_buffer, 1e-3f) + h.fixed_dt;
        if (action_released(h, in, m->jump_action)) m->jump_held = false;
        if (const auto* a = h.options.input_map.find(m->jump_action); a && !gameplay::InputMap::down(in, *a))
            m->jump_held = false;
    }
}

void health_tick(Scene& s, float dt) {
    s.world().each<HealthComponent>([&](Entity e, HealthComponent& hp) {
        if (!hp.has_respawn) {
            hp.respawn_point = world_pos(s, e);
            hp.has_respawn = true;
            hp.current = std::min(hp.current, hp.max);
        }
        const bool was = hp.invulnerable > 0;
        hp.invulnerable = std::max(0.0f, hp.invulnerable - dt);
        if (!hp.blink || (!was && hp.invulnerable <= 0)) return;
        const bool show = hp.invulnerable <= 0 || std::fmod(hp.invulnerable, 0.2f) > 0.1f;
        if (auto* sp = s.world().get<scene::SpriteComponent>(e)) sp->tint.a = show ? 1.0f : 0.25f;
        if (auto* mr = s.world().get<scene::MeshRendererComponent>(e)) mr->visible = show;
    });
}

float falling_speed(Scene& s, physics::PhysicsWorld* pw, Entity e) {
    if (has_character(pw, e)) return pw->character_state(e).velocity.y;
    if (pw && pw->has_body(e)) return pw->linear_velocity(e).y;
    (void)s;
    return -1;
}

void bounce(Scene& s, physics::PhysicsWorld* pw, Entity e, Vec3 v, bool keep_x) {
    if (pw && (has_character(pw, e) || pw->has_body(e))) {
        Vec3 cur = has_character(pw, e) ? pw->character_state(e).velocity : pw->linear_velocity(e);
        if (keep_x) v.x = cur.x, v.z = cur.z;
        pw->set_linear_velocity(e, v);
    }
    if (auto* m = s.world().get<PlatformerMovementComponent>(e)) {
        if (!keep_x) m->velocity_x = v.x;
        m->jumps_used = 1; // a stomp gives the air jumps back
        m->jump_cut_ready = false;
    }
    if (auto* td = s.world().get<TopDownMovementComponent>(e); td && !keep_x) td->velocity = {v.x, td->velocity.y, v.z};
}

void jump_pads(ScriptHost::Impl& h, Scene& s, physics::PhysicsWorld* pw, TagCache& tags, float dt) {
    std::vector<Entity> list;
    s.world().each<JumpPadComponent>([&](Entity e, JumpPadComponent&) { list.push_back(e); });
    for (const Entity pad : list) {
        auto* jp = s.world().get<JumpPadComponent>(pad);
        if (jp->cooldown > 0) jp->cooldown -= dt;
        if (jp->cooldown > 0) continue;
        const JumpPadComponent cfg = *jp;
        Box pb = bounds_of(s, pad);
        pb.h += Vec3{0.1f}; // standing on a solid pad touches it
        const Vec3 dir = math::length(cfg.direction) > 1e-4f ? math::normalize(cfg.direction) : Vec3{0, 1, 0};
        bool launched = false;
        for (const Entity t : tags.get(cfg.target_tag)) {
            if (t == pad || !s.world().alive(t)) continue;
            if (!overlaps(pb, bounds_of(s, t)) || falling_speed(s, pw, t) > cfg.strength * 0.5f) continue;
            const bool straight_up = std::fabs(dir.x) < 1e-3f && std::fabs(dir.z) < 1e-3f;
            bounce(s, pw, t, dir * cfg.strength, straight_up);
            Value p = Value::object();
            p.set("strength", cfg.strength);
            gameplay::emit_event(s, "bounced", std::move(p), pad, t);
            launched = true;
        }
        if (!launched) continue;
        s.world().get<JumpPadComponent>(pad)->cooldown = 0.2f;
        if (!cfg.sound.empty())
            if (auto* a = audio::engine_of(s)) {
                audio::OneShot o;
                o.position = pb.c;
                if (auto st = a->play_one_shot(cfg.sound, o); !st) warn_once(h, pad, std::format("Jump pad sound '{}': {}", cfg.sound, st.error().message));
            }
    }
}

void hazards(ScriptHost::Impl& h, Scene& s, physics::PhysicsWorld* pw, TagCache& tags, std::vector<Entity>& doomed) {
    std::vector<Entity> list;
    s.world().each<HazardComponent>([&](Entity e, HazardComponent&) { list.push_back(e); });
    for (const Entity hz : list) {
        if (!s.world().alive(hz) || std::find(doomed.begin(), doomed.end(), hz) != doomed.end()) continue;
        if (const auto* own = s.world().get<HealthComponent>(hz); own && own->dead) continue;
        const HazardComponent cfg = *s.world().get<HazardComponent>(hz);
        const Box hb = bounds_of(s, hz);
        for (const Entity t : tags.get(cfg.target_tag)) {
            if (t == hz || !s.world().alive(t) || !s.world().has<HealthComponent>(t)) continue;
            if (s.world().get<HealthComponent>(t)->dead) continue;
            const Box tb = bounds_of(s, t);
            if (!overlaps(hb, tb)) continue;
            const bool from_above = tb.c.y - tb.h.y * 0.5f > hb.c.y && falling_speed(s, pw, t) <= 0.5f;
            if (cfg.stompable && from_above) {
                bounce(s, pw, t, {0, cfg.stomp_bounce, 0}, true);
                if (s.world().has<HealthComponent>(hz)) {
                    auto* own = s.world().get<HealthComponent>(hz);
                    own->invulnerable = 0;
                    (void)damage(h, s, pw, hz, own->max, t, doomed);
                } else {
                    doomed.push_back(hz);
                }
                Value p = Value::object();
                p.set("stomped", name_of(s, hz));
                gameplay::emit_event(s, "stomped", std::move(p), t, t);
                break;
            }
            const float before = s.world().get<HealthComponent>(t)->current;
            if (cfg.instant_kill) {
                auto* victim = s.world().get<HealthComponent>(t);
                victim->invulnerable = 0;
                (void)damage(h, s, pw, t, victim->current, hz, doomed);
                continue;
            }
            if (!damage(h, s, pw, t, cfg.damage, hz, doomed)) continue;
            const auto* after = s.world().get<HealthComponent>(t);
            const bool hurt = after && !after->dead && after->current < before; // not respawned
            if (cfg.knockback > 0 && hurt) {
                Vec3 away = tb.c - hb.c;
                if (tb.flat || hb.flat) away.z = 0;
                if (s.world().has<physics::CharacterController2DComponent>(t) &&
                    !s.world().get<physics::CharacterController2DComponent>(t)->top_down) {
                    away = {away.x >= 0 ? 1.0f : -1.0f, 0.6f, 0};
                } else {
                    away.y = std::max(away.y, 0.0f);
                    away = math::normalize(away);
                    if (math::length(away) < 0.5f) away = {1, 0, 0};
                }
                bounce(s, pw, t, away * cfg.knockback, false);
            }
            if (cfg.destroy_on_touch) {
                doomed.push_back(hz);
                break;
            }
        }
    }
}

void collectibles(ScriptHost::Impl& h, Scene& s, TagCache& tags, std::vector<Entity>& doomed) {
    std::vector<Entity> list;
    s.world().each<CollectibleComponent>([&](Entity e, CollectibleComponent&) { list.push_back(e); });
    for (const Entity c : list) {
        if (std::find(doomed.begin(), doomed.end(), c) != doomed.end()) continue;
        const CollectibleComponent cfg = *s.world().get<CollectibleComponent>(c);
        const Box cb = bounds_of(s, c);
        for (const Entity t : tags.get(cfg.collector_tag)) {
            if (t == c || !s.world().alive(t)) continue;
            if (const auto* hp = s.world().get<HealthComponent>(t); hp && hp->dead) continue;
            if (!overlaps(cb, bounds_of(s, t))) continue;
            switch (cfg.kind) {
                case CollectibleKind::Score: add_score(s, cfg.amount); break;
                case CollectibleKind::Health:
                    if (auto* hp = s.world().get<HealthComponent>(t)) {
                        hp->current = std::min(hp->max, hp->current + cfg.amount);
                        Value p = Value::object();
                        p.set("amount", cfg.amount);
                        p.set("health", hp->current);
                        gameplay::emit_event(s, "healed", std::move(p), c, t);
                    }
                    break;
                case CollectibleKind::Item:
                    if (!cfg.item.empty()) {
                        if (!s.world().has<gameplay::InventoryComponent>(t)) s.world().add<gameplay::InventoryComponent>(t);
                        (void)gameplay::add_item(s, t, cfg.item,
                                                 static_cast<std::uint32_t>(std::max(1.0f, std::round(cfg.amount))));
                    }
                    break;
            }
            Value p = Value::object();
            p.set("kind", cfg.kind == CollectibleKind::Score ? "score" : cfg.kind == CollectibleKind::Health ? "health" : "item");
            p.set("amount", cfg.amount);
            p.set("item", cfg.item);
            gameplay::emit_event(s, "collected", std::move(p), c, t);
            std::string text = cfg.message;
            if (text.empty() && cfg.kind == CollectibleKind::Item && !cfg.item.empty()) text = "You found a " + cfg.item;
            if (!text.empty()) {
                h.behaviours.message = std::move(text);
                h.behaviours.message_left = 2.5f;
                h.behaviours.message_owner = Uuid{};
            }
            if (!cfg.sound.empty())
                if (auto* a = audio::engine_of(s)) {
                    audio::OneShot o;
                    o.position = cb.c;
                    if (auto st = a->play_one_shot(cfg.sound, o); !st) warn_once(h, c, std::format("Collectible sound '{}': {}", cfg.sound, st.error().message));
                }
            doomed.push_back(c);
            break;
        }
    }
}

void checkpoints(Scene& s, TagCache& tags) {
    std::vector<Entity> list;
    s.world().each<CheckpointComponent>([&](Entity e, CheckpointComponent&) { list.push_back(e); });
    for (const Entity c : list) {
        const auto cfg = *s.world().get<CheckpointComponent>(c);
        if (cfg.reached) continue;
        const Box cb = bounds_of(s, c);
        const Vec3 at = world_pos(s, c);
        for (const Entity t : tags.get(cfg.player_tag)) {
            if (t == c || !s.world().alive(t) || !overlaps(cb, bounds_of(s, t))) continue;
            s.world().get<CheckpointComponent>(c)->reached = true;
            if (auto* hp = s.world().get<HealthComponent>(t)) {
                hp->respawn_point = {at.x, at.y, is_2d(s, t) ? world_pos(s, t).z : at.z};
                hp->has_respawn = true;
            }
            gameplay::emit_event(s, "checkpoint", Value::object(), c);
            break;
        }
    }
}

void goals(ScriptHost::Impl& h, Scene& s, TagCache& tags) {
    std::vector<Entity> list;
    s.world().each<GoalComponent>([&](Entity e, GoalComponent&) { list.push_back(e); });
    if (list.empty()) return;
    std::size_t coins = 0;
    s.world().each<CollectibleComponent>([&](CollectibleComponent& c) {
        if (c.kind == CollectibleKind::Score) ++coins;
    });
    for (const Entity g : list) {
        const GoalComponent cfg = *s.world().get<GoalComponent>(g);
        if (cfg.reached || (cfg.require_all_collectibles && coins > 0)) continue;
        const Box gb = bounds_of(s, g);
        bool hit = false;
        for (const Entity t : tags.get(cfg.player_tag))
            if (t != g && s.world().alive(t) && overlaps(gb, bounds_of(s, t))) hit = true;
        if (!hit) continue;
        s.world().get<GoalComponent>(g)->reached = true;
        gameplay::emit_event(s, "goal_reached", Value::object(), g);
        if (cfg.action != GoalAction::Win && !cfg.scene.empty()) {
            request_scene(h, s, g, cfg.scene);
        } else {
            const Entity r = rules_entity(s);
            if (!r.is_null()) end_game(s, r, true);
            else gameplay::emit_event(s, "game_won", Value::object(), g);
        }
    }
}

Entity hud_text(Scene& s, Entity parent, const char* name, Vec2 anchor, Vec2 pivot, Vec2 pos, Vec2 size, float font,
                gameui::TextAlign align);

constexpr const char* kMessageName = "Message (Trigger)";

// A panel near the bottom of the screen (above a typical controls bar): canvas > Box > Text.
void show_message(Scene& s, const std::string& text) {
    const Entity root = s.find_by_name(kMessageName);
    Entity box = root.is_null() || s.children(root).empty() ? Entity{} : s.children(root).front();
    if (text.empty()) {
        if (!box.is_null())
            if (auto* r = s.world().get<gameui::UiRectComponent>(box)) r->visible = false;
        return;
    }
    if (box.is_null()) {
        if (!root.is_null()) s.destroy(root);
        const Entity canvas_e = s.create(kMessageName);
        gameui::UiCanvasComponent canvas;
        canvas.sort_order = 55;
        s.world().add<gameui::UiCanvasComponent>(canvas_e, canvas);
        box = s.create("Box", canvas_e);
        gameui::UiRectComponent r;
        r.anchor_min = r.anchor_max = {0.5f, 1};
        r.pivot = {0.5f, 1};
        r.position = {0, -96};
        r.size = {1000, 72};
        s.world().add<gameui::UiRectComponent>(box, r);
        gameui::UiPanelComponent panel;
        panel.color = {0.06f, 0.07f, 0.1f, 0.82f};
        s.world().add<gameui::UiPanelComponent>(box, panel);
        (void)hud_text(s, box, "Text", {0.5f, 0.5f}, {0.5f, 0.5f}, {0, 0}, {960, 60}, 30, gameui::TextAlign::Center);
    }
    if (auto* r = s.world().get<gameui::UiRectComponent>(box)) r->visible = true;
    for (const Entity c : s.children(box))
        if (auto* t = s.world().get<gameui::UiTextComponent>(c); t && t->text != text) t->text = text;
}

void triggers(ScriptHost::Impl& h, Scene& s, TagCache& tags, std::vector<Entity>& doomed) {
    std::vector<Entity> list;
    s.world().each<TriggerComponent>([&](Entity e, TriggerComponent&) { list.push_back(e); });
    if (list.empty()) return;
    auto& b = h.behaviours;
    const auto say = [&](Entity tr, float time, std::string text) {
        if (text.empty()) return;
        b.message = std::move(text);
        b.message_left = time > 0 ? time : 1e9f;
        b.message_owner = time > 0 ? Uuid{} : s.uuid(tr);
    };
    for (const Entity tr : list) {
        const TriggerComponent cfg = *s.world().get<TriggerComponent>(tr);
        Box box = bounds_of(s, tr);
        box.h += Vec3{0.15f}; // a solid door is touched from the side, never overlapped
        Entity who;
        for (const Entity t : tags.get(cfg.player_tag)) {
            if (t == tr || !s.world().alive(t)) continue;
            if (const auto* hp = s.world().get<HealthComponent>(t); hp && hp->dead) continue;
            if (overlaps(box, bounds_of(s, t))) who = t;
        }
        auto* c = s.world().get<TriggerComponent>(tr);
        const bool entered = !who.is_null() && !c->inside;
        c->inside = !who.is_null();
        if (!entered || (cfg.once && c->fired)) continue;
        if (!cfg.required_item.empty()) {
            if (gameplay::item_count(s, who, cfg.required_item) == 0) {
                say(tr, cfg.message_time, cfg.locked_message.empty() ? "You need a " + cfg.required_item : cfg.locked_message);
                continue;
            }
            if (cfg.consume_item) (void)gameplay::remove_item(s, who, cfg.required_item, 1);
            c = s.world().get<TriggerComponent>(tr);
        }
        c->fired = true;
        say(tr, cfg.message_time, cfg.message);
        const Entity target = cfg.target.empty() ? tr : s.find_by_name(cfg.target);
        if (cfg.action != TriggerAction::None && cfg.action != TriggerAction::Win && target.is_null()) {
            warn_once(h, tr, std::format("Trigger '{}': no object named '{}'", name_of(s, tr), cfg.target));
        } else {
            switch (cfg.action) {
                case TriggerAction::None: break;
                case TriggerAction::Start:
                    if (auto* mp = s.world().get<MovingPlatformComponent>(target)) {
                        mp->triggered = true;
                        mp->started = true;
                    } else {
                        warn_once(h, tr, std::format("Trigger '{}': '{}' has no Moving Platform to start", name_of(s, tr),
                                                     name_of(s, target)));
                    }
                    break;
                case TriggerAction::Remove:
                    if (std::find(doomed.begin(), doomed.end(), target) == doomed.end()) doomed.push_back(target);
                    break;
                case TriggerAction::Win: {
                    const Entity r = rules_entity(s);
                    if (!r.is_null()) end_game(s, r, true);
                    else gameplay::emit_event(s, "game_won", Value::object(), tr);
                    break;
                }
            }
        }
        if (!cfg.sound.empty())
            if (auto* a = audio::engine_of(s)) {
                audio::OneShot o;
                o.position = box.c;
                if (auto st = a->play_one_shot(cfg.sound, o); !st) warn_once(h, tr, std::format("Trigger sound '{}': {}", cfg.sound, st.error().message));
            }
        gameplay::emit_event(s, "triggered", Value::object(), tr, tr);
    }
}

// The newest message (from a trigger or a pickup) stays up for its time, or while its trigger is touched.
void messages(ScriptHost::Impl& h, Scene& s, float dt) {
    auto& b = h.behaviours;
    if (!b.message_owner.is_nil()) {
        const Entity owner = s.find(b.message_owner);
        const auto* oc = owner.is_null() ? nullptr : s.world().get<TriggerComponent>(owner);
        if (!oc || !oc->inside) b.message_left = 0;
    }
    if (b.message_left > 0) b.message_left -= dt;
    const bool show = b.message_left > 0;
    if (!show && !b.message_shown) return;
    b.message_shown = show;
    show_message(s, show ? b.message : std::string{});
}

void projectile_hits(ScriptHost::Impl& h, Scene& s, physics::PhysicsWorld* pw, TagCache& tags, float dt,
                     std::vector<Entity>& doomed) {
    std::vector<Entity> list;
    s.world().each<ProjectileComponent>([&](Entity e, ProjectileComponent&) { list.push_back(e); });
    std::vector<Solid> solids;
    bool solids_ready = false;
    for (const Entity p : list) {
        auto* pc = s.world().get<ProjectileComponent>(p);
        pc->age += dt;
        if (pc->age >= pc->lifetime) {
            doomed.push_back(p);
            continue;
        }
        const ProjectileComponent cfg = *pc;
        const Box pb = bounds_of(s, p);
        if (cfg.stop_at_walls) {
            if (!solids_ready) solids = gather_solids(s), solids_ready = true;
            if (hits_solid(solids, pb, p, false)) {
                doomed.push_back(p);
                continue;
            }
        }
        for (const Entity t : tags.get(cfg.target_tag)) {
            if (t == p || !s.world().alive(t) || !s.world().has<HealthComponent>(t)) continue;
            if (s.world().get<HealthComponent>(t)->dead || !overlaps(pb, bounds_of(s, t))) continue;
            (void)damage(h, s, pw, t, cfg.damage, p, doomed);
            if (cfg.destroy_on_hit) {
                doomed.push_back(p);
                break;
            }
        }
    }
}

void lifetimes(Scene& s, float dt, std::vector<Entity>& doomed) {
    s.world().each<LifetimeComponent>([&](Entity e, LifetimeComponent& l) {
        l.age += dt;
        if (l.age >= l.seconds) doomed.push_back(e);
    });
}

Result<Entity> spawn_prefab(ScriptHost::Impl& h, Scene& s, const std::string& path, Vec3 at) {
    auto it = h.prefabs.find(path);
    if (it == h.prefabs.end()) {
        auto loaded = h.options.prefab_loader ? h.options.prefab_loader(path) : read_prefab(h.options.assets_root, path);
        if (!loaded) return std::move(loaded).error();
        it = h.prefabs.emplace(path, std::move(*loaded)).first;
    }
    scene::InstantiateOptions o;
    o.position = at;
    return scene::instantiate(s, it->second.prefab, it->second.asset, o);
}

void spawners(ScriptHost::Impl& h, Scene& s, float dt) {
    std::vector<Entity> list;
    s.world().each<SpawnerComponent>([&](Entity e, SpawnerComponent&) { list.push_back(e); });
    for (const Entity sp : list) {
        auto* c = s.world().get<SpawnerComponent>(sp);
        if (!c->started) {
            c->started = true;
            c->timer = c->spawn_on_start ? c->interval : 0;
        }
        std::erase_if(c->alive, [&](const Uuid& u) { return s.find(u).is_null(); });
        c->timer += dt;
        if (c->timer < c->interval || c->alive.size() >= c->max_alive) continue;
        c->timer = 0;
        if (c->prefab.empty()) {
            warn_once(h, sp, std::format("Spawner on '{}' has no prefab to create", name_of(s, sp)));
            continue;
        }
        const std::string path = c->prefab;
        const float radius = c->radius;
        Vec3 at = world_pos(s, sp);
        if (radius > 0) {
            const float a = random01(h) * math::kTau, r = radius * std::sqrt(random01(h));
            if (is_2d(s, sp)) at += Vec3{std::cos(a) * r, std::sin(a) * r, 0};
            else at += Vec3{std::cos(a) * r, 0, std::sin(a) * r};
        }
        auto e = spawn_prefab(h, s, path, at);
        if (!e) {
            warn_once(h, sp, std::format("Spawner on '{}': cannot create '{}': {}", name_of(s, sp), path, e.error().message));
            continue;
        }
        c = s.world().get<SpawnerComponent>(sp);
        c->alive.push_back(s.uuid(*e));
        ++c->spawned;
        Value p = Value::object();
        p.set("entity", s.uuid(*e).to_string());
        gameplay::emit_event(s, "spawned", std::move(p), sp, sp);
    }
}

void shooters(ScriptHost::Impl& h, Scene& s, TagCache& tags, float dt, bool active) {
    std::vector<Entity> list;
    s.world().each<ShooterComponent>([&](Entity e, ShooterComponent&) { list.push_back(e); });
    for (const Entity sh : list) {
        auto* c = s.world().get<ShooterComponent>(sh);
        c->timer = std::max(0.0f, c->timer - dt);
        if (!active || c->timer > 0) continue;
        const ShooterComponent cfg = *c;
        const Vec3 origin = world_pos(s, sh);
        Vec3 dir = math::normalize(cfg.direction);
        bool fire = false;
        if (cfg.automatic) {
            float best = 1e30f;
            for (const Entity t : tags.get(cfg.target_tag)) {
                if (t == sh || !s.world().alive(t)) continue;
                const Vec3 to = bounds_of(s, t).c - origin;
                const float d = math::length(to);
                if (d < best && d < cfg.speed * 1.5f + 8.0f) best = d, dir = math::normalize(to), fire = true;
            }
        } else {
            fire = action_pressed(h, input_for(h, sh), cfg.action);
            if (const auto* pm = s.world().get<PlatformerMovementComponent>(sh)) dir = {pm->facing, 0, 0};
            else if (const auto* td = s.world().get<TopDownMovementComponent>(sh)) dir = td->facing;
        }
        if (!fire) continue;
        const bool flat = is_2d(s, sh);
        if (flat) dir.z = 0;
        if (math::length(dir) < 1e-3f) dir = {1, 0, 0};
        dir = math::normalize(dir);
        Vec3 at = origin + dir * cfg.spawn_distance;
        if (const auto* cc = s.world().get<physics::CharacterControllerComponent>(sh)) at.y += cc->height * 0.55f;
        s.world().get<ShooterComponent>(sh)->timer = cfg.cooldown;
        Entity bullet;
        if (!cfg.prefab.empty()) {
            auto e = spawn_prefab(h, s, cfg.prefab, at);
            if (!e) {
                warn_once(h, sh, std::format("Shooter on '{}': cannot create '{}': {}", name_of(s, sh), cfg.prefab, e.error().message));
                continue;
            }
            bullet = *e;
        } else {
            bullet = s.create("Bullet");
            s.world().get<Transform>(bullet)->position = at;
            if (flat) {
                scene::SpriteComponent sprite;
                sprite.size = {0.3f, 0.3f};
                sprite.tint = {1.0f, 0.85f, 0.3f, 1};
                sprite.layer = 5;
                sprite.unlit = true;
                s.world().add<scene::SpriteComponent>(bullet, sprite);
            } else {
                scene::MeshRendererComponent mr;
                mr.primitive = "sphere";
                mr.base_color = {1.0f, 0.8f, 0.3f, 1};
                mr.cast_shadows = false;
                s.world().add<scene::MeshRendererComponent>(bullet, mr);
                s.world().get<Transform>(bullet)->scale = Vec3{0.25f};
            }
        }
        auto& pc = s.world().has<ProjectileComponent>(bullet) ? *s.world().get<ProjectileComponent>(bullet)
                                                              : s.world().add<ProjectileComponent>(bullet);
        pc.velocity = dir * cfg.speed;
        pc.damage = cfg.damage;
        pc.target_tag = cfg.target_tag;
        Value p = Value::object();
        p.set("entity", s.uuid(bullet).to_string());
        gameplay::emit_event(s, "fired", std::move(p), sh, sh);
    }
}

// ---- rules and HUD -------------------------------------------------------------------------------

Entity hud_text(Scene& s, Entity parent, const char* name, Vec2 anchor, Vec2 pivot, Vec2 pos, Vec2 size, float font,
                gameui::TextAlign align) {
    const Entity e = s.create(name, parent);
    gameui::UiRectComponent r;
    r.anchor_min = r.anchor_max = anchor;
    r.pivot = pivot;
    r.position = pos;
    r.size = size;
    s.world().add<gameui::UiRectComponent>(e, r);
    gameui::UiTextComponent t;
    t.text = "";
    t.font_size = font;
    t.bold = true;
    t.align = align;
    s.world().add<gameui::UiTextComponent>(e, t);
    return e;
}

void build_hud(Scene& s, Entity rules) {
    const Entity old = s.find_by_name(kHudName);
    if (!old.is_null()) s.destroy(old);
    const Entity hud = s.create(kHudName);
    gameui::UiCanvasComponent canvas;
    canvas.sort_order = 50;
    s.world().add<gameui::UiCanvasComponent>(hud, canvas);
    const Entity score = hud_text(s, hud, "Score", {0, 0}, {0, 0}, {28, 20}, {420, 48}, 34, gameui::TextAlign::Left);
    const Entity lives = hud_text(s, hud, "Lives", {1, 0}, {1, 0}, {-28, 20}, {320, 48}, 34, gameui::TextAlign::Right);
    const Entity time = hud_text(s, hud, "Time", {0.5f, 0}, {0.5f, 0}, {0, 20}, {240, 48}, 34, gameui::TextAlign::Center);
    const Entity banner = hud_text(s, hud, "Banner", {0.5f, 0.5f}, {0.5f, 0.5f}, {0, -20}, {1100, 140}, 84,
                                   gameui::TextAlign::Center);
    if (auto* r = s.world().get<gameui::UiRectComponent>(banner)) r->visible = false;
    const Entity items = hud_text(s, hud, "Items", {0, 0}, {0, 0}, {28, 70}, {640, 40}, 26, gameui::TextAlign::Left);
    auto* g = s.world().get<GameRulesComponent>(rules);
    g->hud = hud;
    g->score_text = score;
    g->lives_text = lives;
    g->time_text = time;
    g->banner_text = banner;
    g->items_text = items;
}

void set_text(Scene& s, Entity e, std::string text, bool visible = true) {
    if (e.is_null() || !s.world().alive(e)) return;
    if (auto* t = s.world().get<gameui::UiTextComponent>(e); t && t->text != text) t->text = std::move(text);
    if (auto* r = s.world().get<gameui::UiRectComponent>(e)) r->visible = visible;
}

void rules_tick(ScriptHost::Impl& h, Scene& s, float dt) {
    const Entity r = rules_entity(s);
    if (r.is_null()) return;
    auto* g = s.world().get<GameRulesComponent>(r);
    if (!g->initialised) {
        g->initialised = true;
        g->lives_left = g->lives;
        g->score = 0;
        g->elapsed = 0;
        g->state = GameState::Playing;
        if (auto c = std::exchange(h.behaviours.carry, std::nullopt); c && g->carry_over) {
            g->score = c->score;
            if (g->time_limit <= 0) g->elapsed = c->elapsed; // a timed level gets its full time
            if (c->lives_left > 0 && g->lives > 0) g->lives_left = std::min(c->lives_left, g->lives);
        }
        h.behaviours.entry = ScriptHost::Impl::Behaviours::Carry{g->score, g->lives_left, g->elapsed};
        if (g->show_hud) build_hud(s, r);
        g = s.world().get<GameRulesComponent>(r);
    }
    if (g->state == GameState::Playing) {
        g->elapsed += dt;
        if (g->score_to_win > 0 && g->score >= g->score_to_win) end_game(s, r, true);
        else if (g->time_limit > 0 && g->elapsed >= g->time_limit) end_game(s, r, false);
        g = s.world().get<GameRulesComponent>(r);
    } else {
        g->end_timer += dt;
        if (g->state == GameState::Lost && g->restart_on_game_over && g->end_timer >= g->restart_delay) {
            g->end_timer = -1e9f; // once
            request_restart(h);
            g = s.world().get<GameRulesComponent>(r);
        }
    }
    if (!g->show_hud || g->hud.is_null()) return;
    const GameRulesComponent v = *g;
    std::string score = "Score " + format_number(v.score);
    if (v.score_to_win > 0) score += " / " + format_number(v.score_to_win);
    set_text(s, v.score_text, score);
    set_text(s, v.lives_text, v.lives > 0 ? std::format("Lives {}", v.lives_left) : std::string{}, v.lives > 0);
    set_text(s, v.time_text, v.time_limit > 0 ? format_time(v.time_limit - v.elapsed) : format_time(v.elapsed));
    {
        // What the player carries (keys and other items), so a locked door makes sense.
        std::string carried;
        TagCache tags{s, {}};
        const auto& players = tags.get("Player");
        if (!players.empty())
            if (const auto* inv = s.world().get<gameplay::InventoryComponent>(players.front())) {
                std::vector<std::pair<std::string, std::uint32_t>> totals;
                for (const auto& st : inv->slots) {
                    if (st.item.empty() || st.count == 0) continue;
                    auto it = std::find_if(totals.begin(), totals.end(), [&](const auto& p) { return p.first == st.item; });
                    if (it == totals.end()) totals.emplace_back(st.item, st.count);
                    else it->second += st.count;
                }
                for (const auto& [item, n] : totals) {
                    if (!carried.empty()) carried += "   ";
                    carried += n > 1 ? std::format("{} x{}", item, n) : item;
                }
            }
        set_text(s, v.items_text, carried, !carried.empty());
    }
    set_text(s, v.banner_text, v.state == GameState::Won ? v.win_message : v.lose_message, v.state != GameState::Playing);
}

void camera_follow(Scene& s, float dt) {
    std::vector<Entity> list;
    s.world().each<CameraFollowComponent>([&](Entity e, CameraFollowComponent&) { list.push_back(e); });
    for (const Entity cam : list) {
        const CameraFollowComponent cfg = *s.world().get<CameraFollowComponent>(cam);
        Entity target = cfg.target.empty() ? Entity{} : s.find_by_name(cfg.target);
        if (target.is_null()) {
            TagCache tags{s, {}};
            const auto& players = tags.get("Player");
            if (!players.empty()) target = players.front();
        }
        if (target.is_null() || target == cam) continue;
        const Box tb = bounds_of(s, target);
        const Vec3 focus = tb.c;
        auto* t = s.world().get<Transform>(cam);
        if (!t) continue;
        const Vec3 now = world_pos(s, cam);
        Vec3 want = focus + cfg.offset;
        if (!cfg.follow_x) want.x = now.x;
        if (!cfg.follow_y) want.y = now.y;
        if (!cfg.follow_z) want.z = now.z;
        auto* c = s.world().get<CameraFollowComponent>(cam);
        const float k = (cfg.smoothing <= 0 || !c->snapped) ? 1.0f : 1.0f - std::exp(-cfg.smoothing * dt);
        c->snapped = true;
        set_world_pos(s, cam, math::lerp(now, want, k));
        const auto* camc = s.world().get<scene::CameraComponent>(cam);
        const bool perspective = camc && camc->projection == scene::CameraComponent::Projection::Perspective;
        if (cfg.look_at && perspective && s.parent(cam).is_null()) {
            const Vec3 f = math::normalize(focus - s.world().get<Transform>(cam)->position);
            if (math::length(f) > 0.5f)
                s.world().get<Transform>(cam)->rotation =
                    math::Quat::euler(std::asin(math::clamp(f.y, -1, 1)), std::atan2(-f.x, -f.z), 0);
        }
    }
}

bool any_behaviour(Scene& s) {
    auto& w = s.world();
    return w.count<PlatformerMovementComponent>() || w.count<TopDownMovementComponent>() || w.count<CameraFollowComponent>() ||
           w.count<HealthComponent>() || w.count<HazardComponent>() || w.count<CollectibleComponent>() ||
           w.count<SpawnerComponent>() || w.count<PatrolComponent>() || w.count<ChaseComponent>() ||
           w.count<MovingPlatformComponent>() || w.count<ProjectileComponent>() || w.count<ShooterComponent>() || w.count<LifetimeComponent>() ||
           w.count<CheckpointComponent>() || w.count<GoalComponent>() || w.count<GameRulesComponent>() ||
           w.count<TriggerComponent>() || w.count<JumpPadComponent>();
}

} // namespace

void behaviours_fixed_update(ScriptHost::Impl& h, Scene& s, float dt) {
    if (!any_behaviour(s)) return;
    VICTUS_PROFILE_SCOPE("behaviours.fixed_update");
    ensure_start_snapshot(h, s);
    physics::PhysicsWorld* pw = physics::world_of(s);
    ensure_controllers(s);
    ensure_platform_bodies(s);
    const bool active = playing(s);
    std::vector<Entity> movers;
    s.world().each<PlatformerMovementComponent>([&](Entity e, PlatformerMovementComponent&) { movers.push_back(e); });
    for (const Entity e : movers) {
        const auto* hp = s.world().get<HealthComponent>(e);
        platformer_step(h, s, pw, e, dt, active && !(hp && hp->dead));
    }
    movers.clear();
    s.world().each<TopDownMovementComponent>([&](Entity e, TopDownMovementComponent&) { movers.push_back(e); });
    for (const Entity e : movers) {
        const auto* hp = s.world().get<HealthComponent>(e);
        top_down_step(h, s, pw, e, dt, active && !(hp && hp->dead));
    }
    TagCache tags{s, {}};
    platforms_move(s, dt, tags, active);
    ai_movement(s, pw, dt, tags, active);
    projectiles_move(s, dt);
}

void behaviours_update(ScriptHost::Impl& h, Scene& s, float dt) {
    if (!any_behaviour(s)) return;
    VICTUS_PROFILE_SCOPE("behaviours.update");
    ensure_start_snapshot(h, s);
    physics::PhysicsWorld* pw = physics::world_of(s);
    const Entity saved = h.current;
    movement_input(h, s);
    health_tick(s, dt);
    rules_tick(h, s, dt);
    const bool active = playing(s);
    std::vector<Entity> doomed;
    {
        TagCache tags{s, {}};
        if (active) {
            hazards(h, s, pw, tags, doomed);
            collectibles(h, s, tags, doomed);
            checkpoints(s, tags);
            goals(h, s, tags);
            triggers(h, s, tags, doomed);
            jump_pads(h, s, pw, tags, dt);
        }
        messages(h, s, dt);
        projectile_hits(h, s, pw, tags, dt, doomed);
    }
    lifetimes(s, dt, doomed);
    if (active) spawners(h, s, dt);
    {
        TagCache tags{s, {}};
        shooters(h, s, tags, dt, active);
    }
    for (const Entity e : doomed)
        if (s.world().alive(e)) s.destroy(e);
    camera_follow(s, dt);
    h.route(saved);
    h.input = h.default_input;
}

} // namespace detail

} // namespace victus::script
