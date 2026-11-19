// SPDX-License-Identifier: Apache-2.0
// Behaviours (ADR-0120): ready-made gameplay components that make a playable game without code.
//
// Add them in the inspector like any other component. Each one does one job and they combine:
// a Player with PlatformerMovement + Health, coins with Collectible, spikes with Hazard, a flag
// with Goal and one GameRules entity that keeps score, lives and the timer and shows them on a
// HUD. They run inside the script host (no Luau needed): movement in the fixed step before
// physics, everything else in the frame update before scripts receive their events, so Luau
// scripts can still react to what behaviours do through `on_event`:
//
//   damaged {amount, health}     to the damaged entity     died {}            to the entity
//   healed {amount, health}      to the healed entity      collected {kind, amount, item}
//   checkpoint {}                broadcast                 score_changed {score}
//   game_won {score}             broadcast                 game_lost {score}  broadcast
//   life_lost {lives}            broadcast                 spawned {entity}   to the spawner
//   bounced {}                   from a Jump Pad           triggered / talked from a Trigger
//   paused / resumed             broadcast                 game_started       after the title
//   new_best_score {score, previous} broadcast
//
// Touching is decided by overlapping bounds (collider, character, sprite or scale), so pickups
// and hazards work with or without colliders and in both 2D (XY) and 3D. "Player" in a target
// tag also matches any entity with PlatformerMovement or TopDownMovement, so beginners do not
// have to tag anything.
//
// Fields marked "runtime" are not saved; they reset when a scene loads.
#pragma once
#include "victus/core/math.hpp"
#include "victus/scene/scene.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace victus::script {

// Side-view running and jumping for a CharacterController2D (or a CharacterController, along X).
// Coyote time lets a jump start just after running off a ledge, the jump buffer remembers a press
// just before landing, max_jumps 2 gives a double jump, and releasing jump early cuts the jump short.
struct PlatformerMovementComponent {
    float speed{6.0f};          // m/s
    float acceleration{60.0f};  // m/s^2 towards the input speed
    float deceleration{70.0f};  // m/s^2 when there is no input
    float jump_speed{9.0f};     // m/s upwards
    std::uint32_t max_jumps{1}; // 2 = double jump
    float coyote_time{0.1f};    // seconds
    float jump_buffer{0.12f};   // seconds
    bool variable_jump{true};   // releasing jump while rising halves the upward speed
    bool flip_sprite{true};     // mirrors the Sprite to face the walking direction
    bool depth_movement{false}; // 3D only: move_y also walks along Z
    std::string move_action{"move_x"};
    std::string jump_action{"jump"};
    std::string down_action{"move_y"}; // held down + jump drops through one-way platforms (2D)
    std::string jump_sound;             // audio clip played on every jump (optional)
    // runtime
    float velocity_x{0};
    float coyote{0};
    float buffer{0};
    std::uint32_t jumps_used{0};
    float facing{1};
    bool jump_held{false};      // jump still held since the last jump started
    bool jump_cut_ready{false}; // the current jump can still be cut short
    float air_time{0};          // seconds off the ground (animation)
};

// Walking in any direction: 2D characters walk in X/Y (the controller is switched to top-down),
// 3D characters walk on the ground (X/Z) and can jump when jump_speed > 0.
struct TopDownMovementComponent {
    float speed{5.0f};
    float acceleration{40.0f};
    float deceleration{50.0f};
    float jump_speed{0.0f};       // 3D only; 0 = no jumping
    bool camera_relative{true};   // 3D: up on the stick walks away from the primary camera
    bool face_movement{true};     // turns the entity (3D) or flips the sprite (2D) to face where it walks
    std::string move_x_action{"move_x"};
    std::string move_y_action{"move_y"};
    std::string jump_action{"jump"};
    // runtime
    math::Vec3 velocity;
    math::Vec3 facing{1, 0, 0};
    float air_time{0};
};

// Keeps a camera (or anything) following a target with smoothing.
struct CameraFollowComponent {
    std::string target{"Player"}; // entity name; empty = the first player-controlled entity
    math::Vec3 offset{0, 3, 8};   // from the target, in world space
    float smoothing{6.0f};        // higher = snappier; 0 = locked to the target
    bool look_at{true};           // perspective cameras turn to look at the target
    bool follow_x{true}, follow_y{true}, follow_z{true};
    // runtime
    bool snapped{false};
};

enum class DeathAction : std::uint8_t { Destroy, Respawn, RestartScene, Nothing };

// Hit points. Hazards, projectiles and health pickups change it; reaching zero runs on_death.
// A player that respawns loses one of GameRules' lives (no lives left = game over).
struct HealthComponent {
    float max{3};
    float current{3};
    float invulnerable_time{1.0f}; // seconds of safety after a hit
    DeathAction on_death{DeathAction::Destroy};
    bool blink{true};              // flashes the Sprite / MeshRenderer while invulnerable
    // runtime
    float invulnerable{0};
    math::Vec3 respawn_point;
    bool has_respawn{false};
    bool dead{false};
};

// Hurts what touches it. Stompable hazards are defeated by landing on them from above.
struct HazardComponent {
    float damage{1};
    std::string target_tag{"Player"};
    float knockback{6.0f};      // m/s pushed away from the hazard
    bool stompable{false};
    float stomp_bounce{8.0f};   // upward speed given to the stomper
    bool destroy_on_touch{false};
    bool instant_kill{false};   // pits and lava: takes all health, even while invulnerable
};

enum class CollectibleKind : std::uint8_t { Score, Health, Item };

// A pickup: adds score (GameRules), restores health or puts an item in the Inventory, then disappears.
struct CollectibleComponent {
    CollectibleKind kind{CollectibleKind::Score};
    float amount{1};
    std::string item;             // Item: item name
    std::string collector_tag{"Player"};
    std::string sound;            // audio clip path played when collected (optional)
    std::string message;          // shown at the bottom of the screen when picked up; items default to "You found a Key"
};

// Creates copies of a prefab every few seconds, up to a limit alive at once.
struct SpawnerComponent {
    std::string prefab;           // .vprefab path under assets/
    float interval{2.0f};
    std::uint32_t max_alive{5};
    float radius{0.0f};           // random offset around the spawner
    bool spawn_on_start{true};
    // runtime
    float timer{0};
    bool started{false};
    std::uint64_t spawned{0};
    std::vector<Uuid> alive;
};

// Moves back and forth between its start and start + offset.
struct PatrolComponent {
    math::Vec3 offset{4, 0, 0};
    float speed{2.0f};
    float wait{0.5f};             // seconds at each end
    bool flip_sprite{true};
    bool turn_at_walls{true};     // turns around when something solid is in the way
    bool turn_at_ledges{false};   // side-view 2D and 3D: turns around instead of walking off an edge
    // runtime
    math::Vec3 origin;
    bool has_origin{false};
    bool returning{false};
    float waiting{0};
};

// A platform (or saw, door, elevator) that travels between its start and start + offset and
// carries whatever stands on it. It becomes a kinematic body, so characters ride it.
struct MovingPlatformComponent {
    math::Vec3 offset{4, 0, 0};
    float speed{2.0f};            // m/s on average
    float wait{0.5f};             // seconds at each end
    bool smooth{true};            // eases in and out at the ends
    bool once{false};             // stops at the far end instead of coming back
    bool wait_for_player{false};  // starts moving the first time the player stands on it
    bool wait_for_trigger{false}; // stays put until a Trigger with action Start names it
    float spin{0.0f};             // degrees per second (around Z in 2D, Y in 3D)
    std::string player_tag{"Player"};
    // runtime
    math::Vec3 origin;
    math::Quat base_rotation;
    bool has_origin{false};
    float progress{0};            // 0 at the start, 1 at start + offset
    bool returning{false};
    float waiting{0};
    bool started{false};
    bool triggered{false};
    float angle{0};
};

// A spring: launches the player (or anything with the tag) that lands on or touches it.
struct JumpPadComponent {
    float strength{16.0f};          // m/s
    math::Vec3 direction{0, 1, 0};  // straight up by default; tilt it for side launches
    std::string target_tag{"Player"};
    std::string sound;              // audio clip played on every bounce (optional)
    // runtime
    float cooldown{0};
};

enum class TriggerAction : std::uint8_t { None, Start, Remove, Win };

// Something happens when the player walks into it: signs show a message, switches start a Moving
// Platform, doors open (Remove) once the player carries the required item, and finish lines win.
struct TriggerComponent {
    std::string player_tag{"Player"};
    std::string message;          // shown at the bottom of the screen; with Press To Use, "Hi!|Bye" is a
                                  // conversation: each press of E shows the next line
    float message_time{3.0f};     // seconds; 0 = while the player is touching it
    std::string required_item;    // Inventory item needed, e.g. "Key" (picked up with a Collectible)
    bool consume_item{true};      // uses the item up
    std::string locked_message;   // shown without the item; empty = "You need a <item>"
    TriggerAction action{TriggerAction::None};
    std::string target;           // entity name the action applies to; empty = this entity
    std::string sound;            // audio clip played when it fires (optional)
    bool once{false};             // fires only the first time
    bool press_to_use{false};     // waits for the "interact" action (E) while the player touches it
    std::string prompt;           // shown while waiting; empty = "Press E"
    // runtime
    bool inside{false};
    bool fired{false};
    bool used{false};             // pressed during this visit
    int line{0};                  // next conversation line
};

// Moves towards a target in range (and stops chasing once it gets far enough away).
struct ChaseComponent {
    std::string target_tag{"Player"};
    float detect_radius{6.0f};
    float lose_radius{10.0f};
    float speed{3.0f};
    float stop_distance{0.6f};
    // runtime
    bool chasing{false};
};

// Flies in a straight line and damages the first matching entity with Health it touches.
struct ProjectileComponent {
    math::Vec3 velocity{10, 0, 0};
    float lifetime{3.0f};
    float damage{1};
    std::string target_tag{"Enemy"};
    bool destroy_on_hit{true};
    bool stop_at_walls{true};     // destroyed when it hits something solid
    // runtime
    float age{0};
};

// Fires projectiles when an input action is pressed (or automatically every cooldown).
struct ShooterComponent {
    std::string prefab;           // projectile prefab; empty = a small built-in bullet
    std::string action{"fire"};
    bool automatic{false};        // fires on its own, without input (turrets)
    float cooldown{0.3f};
    float speed{12.0f};
    float damage{1};
    std::string target_tag{"Enemy"};
    math::Vec3 direction{1, 0, 0}; // used when the entity has no movement behaviour to face with
    float spawn_distance{0.7f};
    // runtime
    float timer{0};
};

// Destroys the entity after a number of seconds.
struct LifetimeComponent {
    float seconds{2.0f};
    // runtime
    float age{0};
};

// Touching it makes it the place the player respawns at.
struct CheckpointComponent {
    std::string player_tag{"Player"};
    // runtime
    bool reached{false};
};

enum class GoalAction : std::uint8_t { Win, NextScene, LoadScene };

// The end of a level. With require_all_collectibles the goal only works once every Score
// collectible is picked up.
struct GoalComponent {
    GoalAction action{GoalAction::Win};
    std::string scene;            // LoadScene / NextScene: scene path, e.g. "scenes/level2.vscene"
    bool require_all_collectibles{false};
    std::string player_tag{"Player"};
    // runtime
    bool reached{false};
};

enum class GameState : std::uint8_t { Playing, Won, Lost };

// Score, lives, a time limit and the HUD that shows them. One per scene.
struct GameRulesComponent {
    float score_to_win{0};        // 0 = no score goal
    std::uint32_t lives{3};       // 0 = unlimited
    float time_limit{0};          // seconds; 0 = no limit
    bool show_hud{true};
    bool restart_on_game_over{true};
    float restart_delay{2.5f};
    std::string win_message{"You win!"};
    std::string lose_message{"Game over"};
    // When a Goal loads another scene, that scene's Game Rules start with this score, lives and
    // time instead of fresh ones. Game over restarts the level with the score it was entered with.
    bool carry_over{true};
    bool allow_pause{true};       // the Pause control (Escape / Start) pauses and resumes
    std::string pause_message{"Paused"};
    // A title screen: shown big over the paused game until the player presses Jump, Fire or
    // Interact. Only once per play session (not after a restart or on the next level).
    std::string title;
    std::string start_prompt;     // empty = "Press Space to start"
    // The best score is saved on the player's computer and shown next to the score.
    bool keep_best_score{true};
    bool health_bar{true};        // a bar under the lives for a player with more than 1 health
    // runtime
    bool showing_title{false};
    float best_score{0};
    bool best_recorded{false};    // this round's score was compared with the best
    float score{0};
    std::uint32_t lives_left{0};
    float elapsed{0};
    GameState state{GameState::Playing};
    float end_timer{0};
    bool initialised{false};
    scene::Entity hud;
    scene::Entity score_text, lives_text, time_text, banner_text, items_text;
    scene::Entity health_back, health_fill;
};

// Registers the components above (called by script::register_components).
void register_behaviour_components();

// Names of every behaviour component, in inspector order.
[[nodiscard]] const std::vector<std::string>& behaviour_names();

} // namespace victus::script
