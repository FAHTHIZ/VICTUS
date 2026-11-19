// SPDX-License-Identifier: Apache-2.0
// Behaviours (ADR-0120): no-code gameplay components played headlessly with real physics.
#include "victus/gameplay/framework.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/gameui/gameui.hpp"
#include "victus/physics/physics.hpp"
#include "victus/scene/prefab.hpp"
#include "victus/scene/reflect.hpp"
#include "victus/script/behaviours.hpp"
#include "victus/script/script.hpp"

#include "doctest.h"

#include <filesystem>
#include <memory>
#include <string>

using namespace victus;
using math::Vec2;
using math::Vec3;
using scene::Entity;
using scene::Scene;
using scene::Transform;
using namespace victus::script;

namespace {

struct Project {
    std::filesystem::path root;
    Project() {
        root = std::filesystem::temp_directory_path() / ("victus_behaviours_" + Uuid::generate().to_string().substr(0, 8));
        std::filesystem::create_directories(root / "assets" / "prefabs");
        std::filesystem::create_directories(root / "scenes");
    }
    ~Project() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

// A headless game: script host (behaviours), optional physics, scripted keyboard.
struct Game {
    Project project;
    std::unique_ptr<ScriptHost> host;
    std::unique_ptr<physics::PhysicsWorld> world;
    gameplay::Simulation sim;
    gameplay::ManualInput input;
    Scene scene;
    std::vector<std::string> loaded;

    explicit Game(bool with_physics = true) {
        script::register_components();
        physics::register_components();
        gameui::register_components();
        HostOptions o;
        o.assets_root = project.root / "assets";
        o.on_scene_loaded = [this](Scene&, std::string_view path) {
            loaded.emplace_back(path);
            if (world) world->clear();
        };
        auto h = ScriptHost::create(o);
        REQUIRE(h.ok());
        host = std::move(h).value();
        script::add_script_systems(sim, *host);
        if (with_physics) {
            auto pw = physics::PhysicsWorld::create();
            REQUIRE(pw.ok());
            world = std::move(pw).value();
            physics::add_physics_systems(sim, *world);
        }
        sim.input = &input;
    }
    void run(int frames) {
        for (int i = 0; i < frames; ++i) {
            sim.tick(scene, 1.0f / 60);
            input.end_frame();
        }
    }
    void hold(const char* key, int frames) {
        input.set_key(key, true);
        run(frames);
        input.set_key(key, false);
    }
    void tap(const char* key) {
        input.set_key(key, true);
        run(1);
        input.set_key(key, false);
    }
    Vec3 pos(Entity e) { return scene.world().get<Transform>(e)->position; }
    Vec3 pos(const char* name) { return pos(scene.find_by_name(name)); }

    Entity ground_2d(Vec2 at, Vec2 size) {
        const Entity e = scene.create("Ground");
        scene.world().get<Transform>(e)->position = {at.x, at.y, 0};
        physics::Collider2DComponent c;
        c.size = size;
        scene.world().add<physics::Collider2DComponent>(e, c);
        return e;
    }
    Entity sprite(const char* name, Vec2 at, Vec2 size = {1, 1}) {
        const Entity e = scene.create(name);
        scene.world().get<Transform>(e)->position = {at.x, at.y, 0};
        scene::SpriteComponent sp;
        sp.size = size;
        scene.world().add<scene::SpriteComponent>(e, sp);
        return e;
    }
    Entity player_2d(Vec2 at) {
        const Entity p = sprite("Player", at, {0.8f, 1.0f});
        scene.world().add<PlatformerMovementComponent>(p);
        return p;
    }
};

} // namespace

TEST_CASE("behaviours: every behaviour is registered, described and round-trips through a scene file") {
    script::register_components();
    for (const std::string& name : behaviour_names()) {
        const auto* schema = scene::find_schema(name);
        REQUIRE_MESSAGE(schema, name);
        CHECK(schema->category == "Behaviours");
        CHECK(!schema->fields.empty());
    }
    Scene s;
    const Entity e = s.create("Hero");
    auto& pm = s.world().add<PlatformerMovementComponent>(e);
    pm.max_jumps = 2;
    pm.speed = 7.5f;
    pm.velocity_x = 3; // runtime: not saved
    auto& hp = s.world().add<HealthComponent>(e);
    hp.on_death = DeathAction::Respawn;
    hp.max = 5;
    auto& g = s.world().add<GameRulesComponent>(e);
    g.score_to_win = 10;
    g.win_message = "Well done";
    s.world().add<GoalComponent>(e).action = GoalAction::LoadScene;
    s.world().add<CollectibleComponent>(e).kind = CollectibleKind::Item;
    const auto doc = s.save();
    Scene t;
    REQUIRE(t.load(doc).ok());
    const Entity f = t.find_by_name("Hero");
    REQUIRE(!f.is_null());
    const auto* pm2 = t.world().get<PlatformerMovementComponent>(f);
    REQUIRE(pm2);
    CHECK(pm2->max_jumps == 2);
    CHECK(pm2->speed == doctest::Approx(7.5f));
    CHECK(pm2->velocity_x == 0);
    CHECK(t.world().get<HealthComponent>(f)->on_death == DeathAction::Respawn);
    CHECK(t.world().get<HealthComponent>(f)->max == doctest::Approx(5));
    CHECK(t.world().get<GameRulesComponent>(f)->win_message == "Well done");
    CHECK(t.world().get<GoalComponent>(f)->action == GoalAction::LoadScene);
    CHECK(t.world().get<CollectibleComponent>(f)->kind == CollectibleKind::Item);
}

TEST_CASE("behaviours: platformer movement runs, jumps, double jumps and drops a short hop when released") {
    Game g;
    g.ground_2d({0, -0.5f}, {80, 1});
    const Entity p = g.player_2d({0, 1});
    g.run(60);
    // A CharacterController2D was added automatically and the player stands on the ground.
    REQUIRE(g.scene.world().has<physics::CharacterController2DComponent>(p));
    CHECK(g.world->character_state(p).grounded);
    const float rest_y = g.pos(p).y;

    g.hold("D", 60);
    CHECK(g.pos(p).x > 4.0f);
    CHECK(!g.scene.world().get<scene::SpriteComponent>(p)->flip_x);
    g.hold("A", 20);
    CHECK(g.scene.world().get<scene::SpriteComponent>(p)->flip_x); // faces left
    g.run(60);

    // Held jump: a full-height jump.
    float peak_single = rest_y;
    g.input.set_key("Space", true);
    for (int i = 0; i < 60; ++i) {
        g.run(1);
        peak_single = std::max(peak_single, g.pos(p).y);
    }
    g.input.set_key("Space", false);
    g.run(90);
    CHECK(peak_single - rest_y > 2.5f);
    CHECK(g.world->character_state(p).grounded);

    // Tapped jump: cut short.
    float peak_tap = rest_y;
    g.tap("Space");
    for (int i = 0; i < 60; ++i) {
        g.run(1);
        peak_tap = std::max(peak_tap, g.pos(p).y);
    }
    g.run(60);
    CHECK(peak_tap - rest_y > 0.3f);
    CHECK(peak_tap - rest_y < (peak_single - rest_y) * 0.8f);

    // With max_jumps 1 a second press in the air does nothing; with 2 it jumps again.
    auto air_peak = [&](std::uint32_t jumps) {
        g.scene.world().get<PlatformerMovementComponent>(p)->max_jumps = jumps;
        float peak = rest_y;
        g.input.set_key("Space", true);
        for (int i = 0; i < 18; ++i) g.run(1);
        g.input.set_key("Space", false);
        g.run(1);
        g.input.set_key("Space", true);
        for (int i = 0; i < 70; ++i) {
            g.run(1);
            peak = std::max(peak, g.pos(p).y);
        }
        g.input.set_key("Space", false);
        g.run(120);
        return peak - rest_y;
    };
    const float one = air_peak(1);
    const float two = air_peak(2);
    CHECK(two > one + 1.0f);
}

TEST_CASE("behaviours: coins add score, the HUD shows it and reaching the target wins") {
    Game g;
    g.ground_2d({0, -0.5f}, {80, 1});
    g.player_2d({0, 1});
    for (int i = 0; i < 3; ++i) {
        const Entity c = g.sprite("Coin", {2.0f + 1.5f * static_cast<float>(i), 0.8f}, {0.5f, 0.5f});
        g.scene.world().add<CollectibleComponent>(c);
    }
    const Entity rules = g.scene.create("Rules");
    g.scene.world().add<GameRulesComponent>(rules).score_to_win = 3;
    std::vector<std::string> events;
    gameplay::listen(g.scene, "", [&](Scene&, const gameplay::GameplayEvent& ev) { events.push_back(ev.name); });
    g.run(30);
    const Entity hud = g.scene.find_by_name("HUD (GameRules)");
    REQUIRE(!hud.is_null());
    CHECK(g.scene.world().has<gameui::UiCanvasComponent>(hud));
    const Entity score = g.scene.find_by_name("Score");
    REQUIRE(!score.is_null());
    CHECK(g.scene.world().get<gameui::UiTextComponent>(score)->text == "Score 0 / 3");

    g.hold("D", 90);
    CHECK(g.scene.find_by_name("Coin").is_null());
    const auto* r = g.scene.world().get<GameRulesComponent>(rules);
    CHECK(r->score == doctest::Approx(3));
    CHECK(r->state == GameState::Won);
    CHECK(std::count(events.begin(), events.end(), "collected") == 3);
    CHECK(std::count(events.begin(), events.end(), "game_won") == 1);
    const Entity banner = g.scene.find_by_name("Banner");
    CHECK(g.scene.world().get<gameui::UiTextComponent>(banner)->text == "You win!");
    CHECK(g.scene.world().get<gameui::UiRectComponent>(banner)->visible);
    // Movement stops once the game is over.
    const float x = g.pos("Player").x;
    g.hold("D", 30);
    CHECK(g.pos("Player").x == doctest::Approx(x).epsilon(0.05));
}

TEST_CASE("behaviours: hazards hurt, respawn costs a life and game over restarts the scene") {
    Game g;
    g.ground_2d({0, -0.5f}, {80, 1});
    const Entity p = g.player_2d({0, 1});
    auto& hp = g.scene.world().add<HealthComponent>(p);
    hp.max = hp.current = 1;
    hp.on_death = DeathAction::Respawn;
    const Entity spikes = g.sprite("Spikes", {3, 0.25f}, {1, 0.5f});
    g.scene.world().add<HazardComponent>(spikes);
    const Entity coin = g.sprite("Coin", {-3, 0.8f}, {0.5f, 0.5f});
    g.scene.world().add<CollectibleComponent>(coin);
    const Entity rules = g.scene.create("Rules");
    auto& gr = g.scene.world().add<GameRulesComponent>(rules);
    gr.lives = 2;
    gr.restart_delay = 0.5f;
    g.run(20);
    const float start_x = g.pos(p).x;

    // Pick up the coin, then run into the spikes: one life lost, back at the start.
    g.hold("A", 40);
    CHECK(g.scene.find_by_name("Coin").is_null());
    auto walk_until = [&](auto done) {
        g.input.set_key("D", true);
        for (int i = 0; i < 240 && !done(); ++i) g.run(1);
        g.input.set_key("D", false);
    };
    walk_until([&] { return g.scene.world().get<GameRulesComponent>(rules)->lives_left == 1; });
    CHECK(g.scene.world().get<GameRulesComponent>(rules)->lives_left == 1);
    CHECK(g.scene.world().get<HealthComponent>(p)->current == doctest::Approx(1));
    g.run(1);
    CHECK(std::abs(g.pos(p).x - start_x) < 0.5f); // respawned where play started

    // Again: no lives left -> game over -> the scene restarts as it was when play began.
    g.run(70); // invulnerability wears off
    walk_until([&] { return g.scene.world().get<GameRulesComponent>(rules)->state != GameState::Playing; });
    g.run(1);
    CHECK(g.scene.world().get<GameRulesComponent>(rules)->state == GameState::Lost);
    CHECK(g.scene.world().get<gameui::UiTextComponent>(g.scene.find_by_name("Banner"))->text == "Game over");
    g.run(60);
    REQUIRE(g.loaded.size() == 1);
    CHECK(!g.scene.find_by_name("Coin").is_null()); // the coin is back
    const Entity r2 = g.scene.find_by_name("Rules");
    g.run(5);
    CHECK(g.scene.world().get<GameRulesComponent>(r2)->state == GameState::Playing);
    CHECK(g.scene.world().get<GameRulesComponent>(r2)->lives_left == 2);
    CHECK(g.pos("Player").x == doctest::Approx(start_x).epsilon(0.1));
}

TEST_CASE("behaviours: stomping a patrolling enemy defeats it and bounces the player") {
    Game g;
    g.ground_2d({0, -0.5f}, {80, 1});
    const Entity p = g.player_2d({0, 4});
    g.scene.world().add<HealthComponent>(p).on_death = DeathAction::Respawn;
    const Entity enemy = g.sprite("Slime", {0.2f, 0.5f}, {1, 1});
    auto& hz = g.scene.world().add<HazardComponent>(enemy);
    hz.stompable = true;
    auto& pa = g.scene.world().add<PatrolComponent>(enemy);
    pa.offset = {1, 0, 0};
    pa.speed = 0.5f;
    bool stomped = false;
    gameplay::listen(g.scene, "stomped", [&](Scene&, const gameplay::GameplayEvent&) { stomped = true; });
    float peak_after = 0;
    for (int i = 0; i < 120; ++i) {
        g.run(1);
        if (stomped) peak_after = std::max(peak_after, g.pos(p).y);
    }
    CHECK(stomped);
    CHECK(g.scene.find_by_name("Slime").is_null());
    CHECK(g.scene.world().get<HealthComponent>(p)->current == doctest::Approx(3)); // unhurt
    CHECK(peak_after > 1.5f);                                                      // bounced up
}

TEST_CASE("behaviours: patrol walks between its ends and waits; chase follows a player in range") {
    Game g(false); // plain transforms, no physics
    const Entity guard = g.scene.create("Guard");
    auto& pa = g.scene.world().add<PatrolComponent>(guard);
    pa.offset = {2, 0, 0};
    pa.speed = 2;
    pa.wait = 0.25f;
    const Entity hunter = g.scene.create("Hunter");
    g.scene.world().get<Transform>(hunter)->position = {0, 0, 5};
    auto& ch = g.scene.world().add<ChaseComponent>(hunter);
    ch.detect_radius = 4;
    ch.lose_radius = 6;
    const Entity hero = g.scene.create("Hero");
    g.scene.world().get<Transform>(hero)->position = {6, 0, 8};
    g.scene.world().add<TopDownMovementComponent>(hero).camera_relative = false;

    float max_x = 0;
    for (int i = 0; i < 70; ++i) {
        g.run(1);
        max_x = std::max(max_x, g.pos(guard).x);
    }
    CHECK(max_x == doctest::Approx(2).epsilon(0.02));
    g.run(50);
    CHECK(g.pos(guard).x < 1.0f); // walking back
    CHECK(g.pos(hunter).z == doctest::Approx(5)); // the hero is out of range

    // Walk the hero (no physics: moves its Transform) towards the hunter.
    g.hold("A", 50); // -x
    g.hold("W", 40); // -z
    const Vec3 hero_at = g.pos(hero);
    g.run(120);
    CHECK(math::length(g.pos(hunter) - hero_at) < 1.0f);
    CHECK(g.scene.world().get<ChaseComponent>(hunter)->chasing);
}

TEST_CASE("behaviours: a shooter fires bullets that damage enemies; lifetime cleans them up") {
    Game g(false);
    const Entity hero = g.sprite("Hero", {0, 0});
    g.scene.world().add<TopDownMovementComponent>(hero);
    g.scene.world().add<ShooterComponent>(hero).cooldown = 0.2f;
    const Entity target = g.sprite("Target", {5, 0});
    g.scene.world().add<HealthComponent>(target).max = 2;
    g.scene.world().get<HealthComponent>(target)->current = 2;
    g.scene.world().get<HealthComponent>(target)->invulnerable_time = 0;
    g.scene.world().add<gameplay::TagsComponent>(target).tags = "Enemy";
    g.run(2);
    g.input.set_mouse(0, true);
    g.run(1);
    g.input.set_mouse(0, false);
    g.run(1);
    REQUIRE(!g.scene.find_by_name("Bullet").is_null());
    g.run(40);
    CHECK(g.scene.world().get<HealthComponent>(target)->current == doctest::Approx(1));
    CHECK(g.scene.find_by_name("Bullet").is_null()); // destroyed on hit
    g.input.set_mouse(0, true);
    g.run(1);
    g.input.set_mouse(0, false);
    g.run(40);
    CHECK(g.scene.find_by_name("Target").is_null()); // Health on_death = destroy

    // A missed shot expires after its lifetime.
    g.input.set_mouse(0, true);
    g.run(1);
    g.input.set_mouse(0, false);
    g.run(1);
    CHECK(!g.scene.find_by_name("Bullet").is_null());
    g.run(200);
    CHECK(g.scene.find_by_name("Bullet").is_null());
}

TEST_CASE("behaviours: spawners create prefab copies up to max_alive") {
    Game g(false);
    {
        Scene src;
        const Entity root = src.create("Bat");
        src.world().add<LifetimeComponent>(root).seconds = 100;
        REQUIRE(scene::save_prefab_file(scene::make_prefab(src, root), g.project.root / "assets" / "prefabs" / "bat.vprefab").ok());
    }
    const Entity sp = g.scene.create("Cave");
    auto& c = g.scene.world().add<SpawnerComponent>(sp);
    c.prefab = "prefabs/bat.vprefab";
    c.interval = 0.5f;
    c.max_alive = 3;
    c.radius = 2;
    g.run(2);
    CHECK(g.scene.world().get<SpawnerComponent>(sp)->spawned == 1); // spawn_on_start
    g.run(240);
    CHECK(g.scene.world().get<SpawnerComponent>(sp)->spawned == 3);
    std::size_t bats = 0;
    g.scene.world().each<LifetimeComponent, Transform>([&](LifetimeComponent&, Transform& t) {
        ++bats;
        CHECK(math::length(t.position) <= 2.01f);
    });
    CHECK(bats == 3);
    // Destroying one lets the spawner make another.
    g.scene.destroy(g.scene.find_by_name("Bat"));
    g.run(40);
    CHECK(g.scene.world().get<SpawnerComponent>(sp)->spawned == 4);
}

TEST_CASE("behaviours: goals load the next scene once every coin is collected; checkpoints move respawn") {
    Game g(false);
    {
        Scene next;
        next.create("Level 2 marker");
        REQUIRE(next.save_file(g.project.root / "scenes" / "level2.vscene").ok());
    }
    const Entity hero = g.scene.create("Hero");
    g.scene.world().add<TopDownMovementComponent>(hero).camera_relative = false;
    g.scene.world().add<HealthComponent>(hero);
    const Entity flag = g.scene.create("Flag");
    g.scene.world().get<Transform>(flag)->position = {4, 0, 0};
    auto& goal = g.scene.world().add<GoalComponent>(flag);
    goal.action = GoalAction::NextScene;
    goal.scene = "scenes/level2.vscene";
    goal.require_all_collectibles = true;
    const Entity coin = g.scene.create("Coin");
    g.scene.world().get<Transform>(coin)->position = {0, 0, -4};
    g.scene.world().add<CollectibleComponent>(coin);
    const Entity cp = g.scene.create("Checkpoint");
    g.scene.world().get<Transform>(cp)->position = {2, 0, 0};
    g.scene.world().add<CheckpointComponent>(cp);
    g.run(2);
    g.hold("D", 70); // over the checkpoint and the goal: the coin is still there
    CHECK(g.loaded.empty());
    CHECK(g.scene.world().get<CheckpointComponent>(cp)->reached);
    CHECK(g.scene.world().get<HealthComponent>(hero)->respawn_point.x == doctest::Approx(2));
    g.hold("A", 70);
    g.hold("W", 55);
    CHECK(g.scene.find_by_name("Coin").is_null());
    g.hold("S", 55);
    g.hold("D", 70);
    REQUIRE(g.loaded.size() == 1);
    CHECK(g.loaded[0] == "scenes/level2.vscene");
    CHECK(!g.scene.find_by_name("Level 2 marker").is_null());
}

TEST_CASE("behaviours: top-down 3D movement walks a character relative to the camera; camera follows") {
    Game g;
    const Entity ground = g.scene.create("Ground");
    g.scene.world().get<Transform>(ground)->position = {0, -0.5f, 0};
    physics::ColliderComponent gc;
    gc.size = {80, 1, 80};
    g.scene.world().add<physics::ColliderComponent>(ground, gc);
    const Entity hero = g.scene.create("Player");
    g.scene.world().add<TopDownMovementComponent>(hero).jump_speed = 6;
    const Entity cam = g.scene.create("Camera");
    g.scene.world().add<scene::CameraComponent>(cam);
    g.scene.world().get<Transform>(cam)->position = {0, 3, 8};
    g.scene.world().add<CameraFollowComponent>(cam).smoothing = 0;
    g.run(30);
    REQUIRE(g.scene.world().has<physics::CharacterControllerComponent>(hero));
    g.hold("W", 60); // away from the camera: -Z
    CHECK(g.pos(hero).z < -3.0f);
    CHECK(std::abs(g.pos(hero).x) < 0.3f);
    // Faces where it walks (forward is -Z).
    const Vec3 fwd = g.scene.world().get<Transform>(hero)->rotation.rotate({0, 0, -1});
    CHECK(fwd.z < -0.9f);
    g.run(1);
    CHECK(g.pos(cam).z == doctest::Approx(g.pos(hero).z + 8).epsilon(0.05));
    // Jumping.
    const float y0 = g.pos(hero).y;
    float peak = y0;
    g.input.set_key("Space", true);
    for (int i = 0; i < 40; ++i) {
        g.run(1);
        peak = std::max(peak, g.pos(hero).y);
    }
    g.input.set_key("Space", false);
    CHECK(peak - y0 > 1.0f);
}
