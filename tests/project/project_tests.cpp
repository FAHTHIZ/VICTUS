// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/gameui/gameui.hpp"
#include "victus/gameplay/capture.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/project/project.hpp"
#include "victus/audio/audio.hpp"
#include "victus/animation/animation.hpp"
#include "victus/navigation/navigation.hpp"
#include "victus/particles/particles.hpp"
#include "victus/assets/database.hpp"
#include "victus/physics/physics.hpp"
#include "victus/script/script.hpp"
#include "victus/net/net.hpp"

#include <filesystem>
#include <functional>

using namespace victus;
namespace sfs = std::filesystem;

namespace {
sfs::path temp_dir(const char* tag) {
    return sfs::temp_directory_path() / (std::string("victus_") + tag + "_" + Uuid::generate().to_string().substr(0, 8));
}
} // namespace

TEST_CASE("gameplay: fixed and variable phases, built-in behaviours") {
    project::register_components();
    scene::Scene s;
    const auto e = s.create("spinner");
    s.world().add<gameplay::RotatorComponent>(e) = {{0, 1, 0}, 90.0f};
    const auto o = s.create("bob");
    s.world().get<scene::Transform>(o)->position = {1, 2, 3};
    s.world().add<gameplay::OscillatorComponent>(o) = {{0, 1, 0}, 0.25f, 0.0f, {}, false};

    gameplay::Simulation sim(1.0f / 50.0f);
    gameplay::add_builtin_systems(sim);
    int fixed = 0, update = 0;
    sim.add_system("count.fixed", gameplay::Phase::FixedUpdate, [&](scene::Scene&, const gameplay::FrameTime&) { ++fixed; });
    sim.add_system("count.update", gameplay::Phase::Update, [&](scene::Scene&, const gameplay::FrameTime&) { ++update; });
    for (int i = 0; i < 10; ++i) sim.tick(s, 0.1f); // 1 s total
    CHECK(update == 10);
    CHECK(fixed == 50);
    // 90 deg/s for 1 s around Y: forward (0,0,-1) → (-1,0,0).
    const math::Vec3 f = s.world().get<scene::Transform>(e)->rotation.rotate({0, 0, -1});
    CHECK(f.x == doctest::Approx(-1.0f).epsilon(0.01));
    CHECK(f.z == doctest::Approx(0.0f).scale(1).epsilon(0.01));
    // sin(2*pi*0.25*1.0) = 1 → y = 2 + 1.
    CHECK(s.world().get<scene::Transform>(o)->position.y == doctest::Approx(3.0f).epsilon(0.01));
    CHECK(s.world().get<scene::Transform>(o)->position.x == doctest::Approx(1.0f));
    // Disabled systems do not run.
    sim.find("count.update")->enabled = false;
    sim.tick(s, 0.1f);
    CHECK(update == 10);

    // Behaviour components serialize by name and round-trip.
    const auto doc = s.save();
    scene::Scene t;
    REQUIRE(t.load(doc).ok());
    const auto* r = t.world().get<gameplay::RotatorComponent>(t.find_by_name("spinner"));
    REQUIRE(r);
    CHECK(r->degrees_per_second == doctest::Approx(90.0f));
}

TEST_CASE("project: create from every template, reopen, sandboxing") {
    project::register_components();
    for (const auto& tpl : project::templates()) {
        CAPTURE(tpl.id);
        const auto dir = temp_dir("proj");
        auto p = project::Project::create(dir, "My Game", tpl.id);
        REQUIRE(p.ok());
        CHECK(project::Project::is_project_dir(dir));
        CHECK(sfs::is_directory(dir / "assets"));
        CHECK(sfs::is_regular_file(dir / ".gitignore"));
        auto reopened = project::Project::open(dir / "victus.project");
        REQUIRE(reopened.ok());
        CHECK(reopened->settings().name == "My Game");
        CHECK(reopened->settings().template_id == tpl.id);
        CHECK(reopened->scenes() == std::vector<std::string>{"scenes/main.vscene"});
        auto scene_path = reopened->startup_scene_path();
        REQUIRE(scene_path.ok());
        scene::Scene s;
        REQUIRE(s.load_file(*scene_path).ok());
        CHECK(s.size() > 4);
        bool camera = false;
        s.world().each<scene::CameraComponent>([&](const scene::CameraComponent&) { camera = true; });
        CHECK(camera);
        // No unknown components: every template component is registered.
        bool unknown = false;
        s.world().each<scene::UnknownComponents>([&](const scene::UnknownComponents&) { unknown = true; });
        CHECK_FALSE(unknown);
        CHECK_FALSE(reopened->resolve("../outside").ok());
        CHECK(reopened->relative(dir / "assets" / "a.png") == "assets/a.png");
        CHECK(reopened->relative(dir.parent_path()).empty());
        // A non-empty folder is refused.
        CHECK_FALSE(project::Project::create(dir, "Again", tpl.id).ok());
        sfs::remove_all(dir);
    }
    CHECK_FALSE(project::Project::create(temp_dir("bad"), "X", "nope").ok());
}

TEST_CASE("project: unknown settings keys survive a save") {
    const auto dir = temp_dir("keys");
    auto p = project::Project::create(dir, "Keys", "2d");
    REQUIRE(p.ok());
    auto text = fs::read_text(dir / "victus.project");
    REQUIRE(text.ok());
    auto doc = json::parse(*text);
    REQUIRE(doc.ok());
    json::Value plugin = json::Value::object();
    plugin.set("x", 5);
    doc->set("plugin_settings", std::move(plugin));
    REQUIRE(fs::write_text_atomic(dir / "victus.project", json::stringify(*doc)).ok());
    auto q = project::Project::open(dir);
    REQUIRE(q.ok());
    q->settings().name = "Renamed";
    REQUIRE(q->save().ok());
    auto r = project::Project::open(dir);
    REQUIRE(r.ok());
    CHECK(r->settings().name == "Renamed");
    CHECK(r->settings().extra["plugin_settings"]["x"].as_int(0) == 5);
    sfs::remove_all(dir);
}

TEST_CASE("project: rendering quality settings round-trip and validate") {
    const auto dir = temp_dir("quality");
    auto p = project::Project::create(dir, "Quality", "3d");
    REQUIRE(p.ok());
    CHECK(p->settings().render_quality == "high");
    CHECK(p->settings().render_scale == 0.0f);
    p->settings().render_quality = "low";
    p->settings().render_scale = 0.5f;
    CHECK(p->settings().target_fps == 0);
    p->settings().target_fps = 30;
    p->settings().dynamic_resolution_min = 0.6f;
    REQUIRE(p->save().ok());
    auto q = project::Project::open(dir);
    REQUIRE(q.ok());
    CHECK(q->settings().render_quality == "low");
    CHECK(q->settings().render_scale == 0.5f);
    CHECK(q->settings().target_fps == 30);
    CHECK(q->settings().dynamic_resolution_min == doctest::Approx(0.6f));
    CHECK_FALSE(q->settings().extra.find("rendering")); // a known key, not preserved as "extra"

    auto text = fs::read_text(dir / "victus.project");
    REQUIRE(text.ok());
    auto doc = json::parse(*text);
    REQUIRE(doc.ok());
    json::Value bad = json::Value::object();
    bad.set("quality", "cinematic");
    doc->set("rendering", bad);
    REQUIRE(fs::write_text_atomic(dir / "victus.project", json::stringify(*doc)).ok());
    CHECK_FALSE(project::Project::open(dir).ok());
    bad.set("quality", "medium");
    bad.set("render_scale", 9.0);
    doc->set("rendering", bad);
    REQUIRE(fs::write_text_atomic(dir / "victus.project", json::stringify(*doc)).ok());
    CHECK_FALSE(project::Project::open(dir).ok());
    bad.set("render_scale", 1.0);
    bad.set("target_fps", -5);
    doc->set("rendering", bad);
    REQUIRE(fs::write_text_atomic(dir / "victus.project", json::stringify(*doc)).ok());
    CHECK_FALSE(project::Project::open(dir).ok());
    sfs::remove_all(dir);
}

TEST_CASE("project: 3D template ships an Environment that round-trips") {
    project::register_components();
    scene::Scene s;
    REQUIRE(project::build_template_scene("3d", s).ok());
    const gameplay::EnvironmentComponent* found = nullptr;
    s.world().each<gameplay::EnvironmentComponent>([&](const gameplay::EnvironmentComponent& e) { found = &e; });
    REQUIRE(found);
    CHECK(found->shadow_distance == 30.0f);
    const std::string text = json::stringify(s.save());
    scene::Scene t;
    REQUIRE(t.load(json::parse(text).value()).ok());
    int count = 0;
    t.world().each<gameplay::EnvironmentComponent>([&](const gameplay::EnvironmentComponent& e) {
        ++count;
        CHECK(e.bloom_intensity == doctest::Approx(0.04f));
        CHECK(e.tonemapper == gameplay::EnvironmentComponent::Tonemapper::Aces);
    });
    CHECK(count == 1);
    // Enum values are validated on load.
    std::string broken = text;
    const auto pos = broken.find("\"aces\"");
    REQUIRE(pos != std::string::npos);
    broken.replace(pos, 6, "\"warm\"");
    scene::Scene u;
    CHECK_FALSE(u.load(json::parse(broken).value()).ok());
}

TEST_CASE("project: recent projects list") {
    const auto file = temp_dir("recent") / "recent.json";
    {
        project::RecentProjects r(file);
        CHECK(r.entries().empty());
        r.add("/a");
        r.add("/b");
        r.add("/a");
        REQUIRE(r.save().ok());
    }
    project::RecentProjects r(file);
    REQUIRE(r.entries().size() == 2);
    CHECK(r.entries()[0] == fs::Path("/a"));
    sfs::remove_all(file.parent_path());
}

namespace {
struct TemplateRun {
    scene::Scene scene;
    std::unique_ptr<script::ScriptHost> host;
    std::unique_ptr<physics::PhysicsWorld> physics;
    std::unique_ptr<assets::AssetDatabase> db;
    std::unique_ptr<animation::AnimationSystem> animation;
    std::unique_ptr<particles::ParticleSystem> particles;
    std::unique_ptr<navigation::NavigationSystem> navigation;
    std::unique_ptr<audio::AudioEngine> audio;
    gameplay::Simulation sim;
    gameplay::ManualInput input;
    void frames(int n) {
        for (int i = 0; i < n; ++i) {
            sim.tick(scene, 1.0f / 60.0f);
            input.end_frame();
        }
    }
    math::Vec3 pos(const char* name) {
        const auto e = scene.find_by_name(name);
        REQUIRE_FALSE(e.is_null());
        return scene.world().get<scene::Transform>(e)->position;
    }
    std::string text(const char* name) {
        const auto e = scene.find_by_name(name);
        REQUIRE_FALSE(e.is_null());
        const auto* t = scene.world().get<gameui::UiTextComponent>(e);
        REQUIRE(t);
        return t->text;
    }
    bool ui_visible(const char* name) {
        const auto e = scene.find_by_name(name);
        REQUIRE_FALSE(e.is_null());
        return scene.world().get<gameui::UiRectComponent>(e)->visible;
    }
    // Presses and releases the left mouse button at a point (pixels, 1280x720 headless).
    void click_at(math::Vec2 p) {
        input.set_mouse_position(p);
        input.set_mouse(0, true);
        frames(1);
        input.set_mouse(0, false);
        frames(1);
    }
};

std::unique_ptr<TemplateRun> start_template(const char* id, const sfs::path& dir, bool create = true, std::uint64_t seed = 0) {
    project::register_components();
    auto p = create ? project::Project::create(dir, "Scripted", id) : project::Project::open(dir);
    REQUIRE(p.ok());
    auto r = std::make_unique<TemplateRun>();
    auto path = p->startup_scene_path();
    REQUIRE(path.ok());
    REQUIRE(r->scene.load_file(*path).ok());
    script::HostOptions o;
    o.assets_root = dir / "assets";
    o.input_map = p->settings().input;
    o.save_directory = p->dev_save_directory();
    o.save_version = p->settings().save_version;
    o.random_seed = seed;
    auto h = script::ScriptHost::create(o);
    REQUIRE(h.ok());
    r->host = std::move(h).value();
    gameplay::add_builtin_systems(r->sim);
    gameui::add_ui_systems(r->sim);
    script::add_script_systems(r->sim, *r->host);
    auto w = physics::PhysicsWorld::create();
    REQUIRE(w.ok());
    r->physics = std::move(w).value();
    physics::add_physics_systems(r->sim, *r->physics);
    // Clips come from the project's asset database (imported on demand, no GPU needed).
    auto db = assets::AssetDatabase::open(dir);
    REQUIRE(db.ok());
    r->db = std::move(db).value();
    animation::AnimationSettings anim;
    anim.provider = assets::animation_provider(*r->db);
    r->animation = std::make_unique<animation::AnimationSystem>(anim);
    animation::add_animation_systems(r->sim, *r->animation);
    r->particles = std::make_unique<particles::ParticleSystem>();
    particles::add_particle_systems(r->sim, *r->particles);
    r->navigation = std::make_unique<navigation::NavigationSystem>();
    navigation::add_navigation_systems(r->sim, *r->navigation);
    audio::EngineOptions ao;
    ao.assets_root = dir / "assets";
    ao.null_device = true;
    auto a = audio::AudioEngine::create(ao);
    REQUIRE(a.ok());
    r->audio = std::move(a).value();
    audio::add_audio_systems(r->sim, *r->audio);
    r->sim.input = &r->input;
    return r;
}
} // namespace

TEST_CASE("templates: 2D platformer is playable through its scripts") {
    const auto dir = temp_dir("tpl2d");
    auto r = start_template("2d", dir);
    r->frames(30); // settle: the player starts on the ground
    CHECK(r->pos("Player").y == doctest::Approx(-2.55f).epsilon(4e-3));
    CHECK(r->host->stats().instances == 5); // player + 3 coins + HUD
    CHECK(r->text("Coin Counter") == "Coins 0 / 3");
    CHECK(r->audio->is_playing(r->scene.find_by_name("Music"))); // looping streamed music
    CHECK(r->audio->stats().one_shots == 0);
    // Run right: the player stops at the right wall and collects the coin on the ground.
    r->input.set_key("D", true);
    r->frames(300);
    CHECK(r->pos("Player").x == doctest::Approx(8.8f - 0.43f).epsilon(2e-3)); // capsule radius 0.43
    CHECK(r->pos("Player").y == doctest::Approx(-2.55f).epsilon(4e-3));
    CHECK(r->scene.find_by_name("Coin 3").is_null());
    CHECK_FALSE(r->scene.find_by_name("Coin 1").is_null());
    CHECK(r->text("Coin Counter") == "Coins 1 / 3");
    CHECK(r->audio->stats().one_shots == 1); // coin pickup sound
    // The pickup burst came from the shared Sparkles emitter; remaining coins still glitter.
    CHECK(particles::status(r->scene, r->scene.find_by_name("Sparkles")).emitted == 28);
    CHECK(particles::status(r->scene, r->scene.find_by_name("Glitter")).alive > 0);
    r->input.set_key("D", false);
    // Jump: rises, then lands back on the ground.
    r->input.set_key("Space", true);
    r->frames(10);
    r->input.set_key("Space", false);
    CHECK(r->pos("Player").y > -1.5f);
    CHECK(r->audio->stats().one_shots == 2); // + jump sound (the mixer is not pulled, so none finished)
    r->frames(120);
    CHECK(r->pos("Player").y == doctest::Approx(-2.55f).epsilon(4e-3));
    // Walk left under the low platform, jump onto it and collect Coin 1.
    r->input.set_key("A", true);
    r->frames(60); // x ~ 2.55
    r->input.set_key("A", false);
    r->frames(2);
    const float x = r->pos("Player").x;
    CAPTURE(x);
    r->input.set_key("Left", true);
    r->frames(40);   // to x ~ -1.4, past the platform's left edge
    r->input.set_key("Left", false);
    r->input.set_key("Space", true);
    r->input.set_key("Right", true);
    r->frames(25);   // jump up and right onto the platform
    r->input.set_key("Space", false);
    r->frames(60);
    r->input.set_key("Right", false);
    r->frames(30);
    CHECK(r->scene.find_by_name("Coin 1").is_null());
    CHECK(r->text("Coin Counter") == "Coins 2 / 3");
    CHECK_FALSE(r->ui_visible("Win Banner"));
    // The lantern hangs from its bracket on a live revolute joint.
    const scene::Entity lantern = r->scene.find_by_name("Lantern");
    CHECK(r->physics->joint_state(lantern).active);
    CHECK(r->pos("Lantern").y == doctest::Approx(1.5f).epsilon(2e-2));
    // One-way ledge: from the ground directly below, jump up through it and land on top...
    const scene::Entity player = r->scene.find_by_name("Player");
    r->scene.world().get<scene::Transform>(player)->position = {2.5f, -2.55f, 0};
    r->frames(30);
    REQUIRE(r->pos("Player").y == doctest::Approx(-2.55f).epsilon(4e-3));
    r->input.set_key("Space", true);
    r->frames(5);
    r->input.set_key("Space", false);
    r->frames(90);
    CHECK(r->pos("Player").y == doctest::Approx(-0.8f + 0.45f).epsilon(2e-2));
    CHECK(r->physics->character_state(player).grounded);
    // ...then hold S to drop back through to the ground.
    r->input.set_key("S", true);
    r->frames(5);
    r->input.set_key("S", false);
    r->frames(60);
    CHECK(r->pos("Player").y == doctest::Approx(-2.55f).epsilon(4e-3));
    CHECK(r->host->stats().errors == 0);
    for (const auto& e : r->host->recent_errors()) MESSAGE(e);
    std::error_code ec;
    sfs::remove_all(dir, ec);
}

TEST_CASE("templates: HUD controls panel toggles with its button and H; the 2D banner shows on completion") {
    for (const char* id : {"2d", "3d"}) {
        CAPTURE(id);
        const auto dir = temp_dir(id[0] == '2' ? "tplhud2d" : "tplhud3d");
        auto r = start_template(id, dir);
        r->frames(2);
        CHECK(r->ui_visible("Controls Panel"));
        // The button: anchored top-right, 164x52 at 24 px from the edges of the 1280x720 screen.
        const scene::Entity help = r->scene.find_by_name("Help Button");
        const auto* st = r->scene.world().get<gameui::UiStateComponent>(help);
        REQUIRE(st);
        CHECK(st->rect.x == doctest::Approx(1280 - 24 - 164));
        CHECK(st->rect.y == doctest::Approx(24));
        r->click_at({1170, 50});
        CHECK_FALSE(r->ui_visible("Controls Panel"));
        r->click_at({1170, 50});
        CHECK(r->ui_visible("Controls Panel"));
        // A press outside the button does nothing.
        r->click_at({640, 400});
        CHECK(r->ui_visible("Controls Panel"));
        r->input.set_key("H", true);
        r->frames(1);
        r->input.set_key("H", false);
        r->frames(1);
        CHECK_FALSE(r->ui_visible("Controls Panel"));
        if (id[0] == '2') {
            // Collect every coin by moving the player onto each one: the banner appears.
            const scene::Entity player = r->scene.find_by_name("Player");
            for (const char* coin : {"Coin 1", "Coin 2", "Coin 3"}) {
                const math::Vec3 at = r->pos(coin);
                r->scene.world().get<scene::Transform>(player)->position = at;
                r->frames(3);
            }
            CHECK(r->text("Coin Counter") == "Coins 3 / 3");
            CHECK(r->ui_visible("Win Banner"));
        }
        CHECK(r->host->stats().errors == 0);
        for (const auto& e : r->host->recent_errors()) MESSAGE(e);
        std::error_code ec;
        sfs::remove_all(dir, ec);
    }
}

TEST_CASE("templates: 3D character climbs the steps, jumps and swings the gate") {
    const auto dir = temp_dir("tpl3d_char");
    auto r = start_template("3d", dir);
    r->frames(5);
    const scene::Entity player = r->scene.find_by_name("Player");
    const scene::Entity gate = r->scene.find_by_name("Gate");
    REQUIRE(r->physics->character_state(player).grounded);
    REQUIRE(r->physics->joint_state(gate).active);
    const scene::Entity dust = r->scene.find_by_name("Dust");
    CHECK(particles::status(r->scene, dust).emitted == 0);
    // Jump in place.
    r->input.set_key("Space", true);
    r->frames(1);
    r->input.set_key("Space", false);
    CHECK(particles::status(r->scene, dust).emitted == 14); // take-off puff
    float apex = 0;
    for (int i = 0; i < 40; ++i) {
        r->frames(1);
        apex = std::max(apex, r->pos("Player").y);
    }
    CHECK(apex > 1.5f); // 6 m/s -> 1.83 m
    r->frames(50);
    CHECK(r->physics->character_state(player).grounded);
    CHECK(particles::status(r->scene, dust).emitted == 24); // + landing puff
    // The brazier burns: fire, sparks and smoke are all alive.
    for (const char* fx : {"Fire", "Sparks", "Smoke"}) {
        CAPTURE(fx);
        CHECK(particles::status(r->scene, r->scene.find_by_name(fx)).alive > 0);
    }
    // Walk to the stairs and up all three steps.
    r->input.set_key("W", true);
    r->frames(54); // z 0.5 -> -4
    r->input.set_key("W", false);
    r->input.set_key("A", true);
    r->frames(84); // x 0 -> -7
    r->input.set_key("A", false);
    r->frames(10);
    CHECK(r->pos("Player").y == doctest::Approx(0.9f).epsilon(0.06)); // on top of step 3
    CHECK(r->physics->character_state(player).grounded);
    // Walk back down, then push through the gate.
    r->input.set_key("D", true);
    for (int i = 0; i < 120 && r->pos("Player").x < -4.6f; ++i) r->frames(1);
    r->input.set_key("D", false);
    CHECK(r->pos("Player").x >= -4.6f);
    r->input.set_key("S", true);
    r->frames(150); // z -4 -> past the gate at z = 4
    r->input.set_key("S", false);
    r->frames(10);
    CHECK(r->pos("Player").z > 4.5f);
    CHECK(r->pos("Player").y == doctest::Approx(0.0f).epsilon(0.05)); // back on the ground
    CHECK(std::abs(r->physics->joint_state(gate).angle) > 30.0f);
    CHECK(r->host->stats().errors == 0);
    for (const auto& e : r->host->recent_errors()) MESSAGE(e);
    std::error_code ec;
    sfs::remove_all(dir, ec);
}

TEST_CASE("templates: 3D player moves with WASD and the camera follows") {
    const auto dir = temp_dir("tpl3d");
    auto r = start_template("3d", dir);
    r->frames(2);
    const math::Vec3 p0 = r->pos("Player"), c0 = r->pos("Main Camera");
    r->input.set_key("W", true);
    r->frames(60);
    const math::Vec3 p1 = r->pos("Player"), c1 = r->pos("Main Camera");
    CHECK(p1.z < p0.z - 4.0f); // ~5 units/s forward (-Z)
    CHECK(p1.x == doctest::Approx(p0.x));
    CHECK(c1.z < c0.z - 2.0f); // smoothed follow
    // The robot walks while moving and faces the direction of travel (-Z: yaw 180 degrees).
    const scene::Entity robot = r->scene.find_by_name("Robot");
    const auto walking = r->animation->status(r->scene, robot);
    CHECK(walking.valid);
    CHECK(walking.clip == "Walk");
    CHECK(walking.animated_nodes > 0);
    const math::Vec3 facing = r->scene.world().get<scene::Transform>(robot)->rotation.rotate({0, 0, 1});
    CHECK(facing.z == doctest::Approx(-1.0f).epsilon(0.01));
    r->frames(240);
    CHECK(r->pos("Player").z == doctest::Approx(-9.0f).epsilon(0.02)); // stops at the bounds
    CHECK(r->animation->status(r->scene, robot).clip == "Idle"); // blocked at the bounds: idles
    CHECK(r->pos("Player").y == doctest::Approx(0.0f).epsilon(0.05)); // standing on the ground
    // The crate stack fell onto the ground collider and came to rest stacked.
    CHECK(r->pos("Crate 1").y == doctest::Approx(0.3f).epsilon(0.03));
    CHECK(r->pos("Crate 3").y > 1.3f);
    CHECK(r->physics->stats().bodies_3d >= 7);
    // Drive the ball through the stack: the crates get knocked over.
    r->input.set_key("W", false);
    const math::Vec3 crate_before = r->pos("Crate 3");
    r->input.set_key("D", true);
    r->frames(43); // x ~ 3.6, in line with the stack at z=2
    r->input.set_key("D", false);
    r->input.set_key("S", true);
    r->frames(200); // roll +Z from z=-9 through the stack
    r->input.set_key("S", false);
    r->frames(60);
    CAPTURE(r->pos("Player"));
    CHECK(math::length(r->pos("Crate 3") - crate_before) > 0.5f);
    CHECK(r->audio->stats().one_shots >= 1); // falling crates played impact sounds
    CHECK(r->audio->stats().load_errors == 0);
    CHECK(r->host->stats().errors == 0);
    for (const auto& e : r->host->recent_errors()) MESSAGE(e);
    std::error_code ec;
    sfs::remove_all(dir, ec);
}

TEST_CASE("templates: gamepads drive the 3D, 2D and multiplayer players through input actions") {
    using gameplay::gamepad_axis_index;
    using gameplay::gamepad_button_index;
    {
        const auto dir = temp_dir("pad3d");
        auto r = start_template("3d", dir);
        r->frames(2);
        const math::Vec3 p0 = r->pos("Player");
        // Full tilt up-right walks at full speed diagonally; half tilt walks slower.
        r->input.set_gamepad_axis(0, gamepad_axis_index("LeftX"), 0.7071f);
        r->input.set_gamepad_axis(0, gamepad_axis_index("LeftY"), -0.7071f);
        r->frames(30);
        const math::Vec3 p1 = r->pos("Player");
        CHECK(p1.x > p0.x + 0.8f);
        CHECK(p1.z < p0.z - 0.8f);
        r->input.set_gamepad_axis(0, gamepad_axis_index("LeftX"), 0.0f);
        r->input.set_gamepad_axis(0, gamepad_axis_index("LeftY"), -1.0f);
        r->frames(30);
        const float full = p1.z - r->pos("Player").z;
        r->input.set_gamepad_axis(0, gamepad_axis_index("LeftY"), -0.6f);
        const math::Vec3 p2 = r->pos("Player");
        r->frames(30);
        const float half = p2.z - r->pos("Player").z;
        CAPTURE(full);
        CAPTURE(half);
        CHECK(full > 2.0f);
        CHECK(half == doctest::Approx(full * 0.5f).epsilon(0.15));
        r->input.set_gamepad_axis(0, gamepad_axis_index("LeftY"), 0.0f);
        r->frames(20);
        // South (A) jumps.
        const float y0 = r->pos("Player").y;
        r->input.set_gamepad_button(0, gamepad_button_index("South"), true);
        r->frames(12);
        CHECK(r->pos("Player").y > y0 + 0.5f);
        r->input.set_gamepad_button(0, gamepad_button_index("South"), false);
        r->frames(60);
        // View/Back toggles the help panel like H.
        const bool shown = r->ui_visible("Controls Panel");
        r->input.set_gamepad_button(0, gamepad_button_index("Back"), true);
        r->frames(1);
        r->input.set_gamepad_button(0, gamepad_button_index("Back"), false);
        r->frames(1);
        CHECK(r->ui_visible("Controls Panel") != shown);
        CHECK(r->host->stats().errors == 0);
        for (const auto& e : r->host->recent_errors()) MESSAGE(e);
        std::error_code ec;
        sfs::remove_all(dir, ec);
    }
    {
        const auto dir = temp_dir("pad2d");
        auto r = start_template("2d", dir);
        r->frames(30);
        const float x0 = r->pos("Player").x;
        r->input.set_gamepad_axis(0, gamepad_axis_index("LeftX"), 1.0f);
        r->frames(30);
        CHECK(r->pos("Player").x > x0 + 1.0f);
        r->input.set_gamepad_axis(0, gamepad_axis_index("LeftX"), 0.0f);
        r->input.set_gamepad_button(0, gamepad_button_index("South"), true);
        r->frames(10);
        r->input.set_gamepad_button(0, gamepad_button_index("South"), false);
        CHECK(r->pos("Player").y > -1.5f);
        r->frames(120);
        CHECK(r->pos("Player").y == doctest::Approx(-2.55f).epsilon(4e-3));
        CHECK(r->host->stats().errors == 0);
        std::error_code ec;
        sfs::remove_all(dir, ec);
    }
    {
        const auto dir = temp_dir("padmp");
        auto r = start_template("multiplayer", dir);
        r->frames(30);
        const math::Vec3 p0 = r->pos("Player");
        r->input.set_gamepad_axis(0, gamepad_axis_index("LeftX"), -1.0f);
        r->frames(30);
        CHECK(r->pos("Player").x < p0.x - 1.0f);
        CHECK(r->host->stats().errors == 0);
        std::error_code ec;
        sfs::remove_all(dir, ec);
    }
}

TEST_CASE("templates: quick save and quick load restore the 2D and 3D games") {
    const auto press = [](TemplateRun& r, const char* key) {
        r.input.set_key(key, true);
        r.frames(1);
        r.input.set_key(key, false);
        r.frames(1);
    };
    const auto toast = [](TemplateRun& r) {
        const auto* rect = r.scene.world().get<gameui::UiRectComponent>(r.scene.find_by_name("Save Status"));
        return rect && rect->visible ? r.text("Save Status") : std::string();
    };
    {
        const auto dir = temp_dir("quicksave2d");
        auto r = start_template("2d", dir);
        r->frames(30);
        CHECK(toast(*r).empty());
        press(*r, "F9");
        CHECK(toast(*r) == "No quick save yet (F5 saves)");
        r->input.set_key("D", true);
        r->frames(300); // collects Coin 3 at the right wall
        r->input.set_key("D", false);
        CHECK(r->text("Coin Counter") == "Coins 1 / 3");
        press(*r, "F5");
        CHECK(toast(*r) == "Game saved");
        CHECK(sfs::exists(dir / ".victus" / "saves" / "quick.vsave"));
        const float saved_x = r->pos("Player").x;
        r->input.set_key("A", true);
        r->frames(90);
        r->input.set_key("A", false);
        CHECK(r->pos("Player").x < saved_x - 3.0f);
        press(*r, "F9");
        CHECK(r->pos("Player").x == doctest::Approx(saved_x).epsilon(1e-3));
        CHECK(r->text("Coin Counter") == "Coins 1 / 3"); // score and total from load_state
        CHECK(r->scene.find_by_name("Coin 3").is_null());
        CHECK_FALSE(r->scene.find_by_name("Coin 1").is_null());
        CHECK(toast(*r) == "Game loaded");
        r->frames(150); // the toast hides after two seconds
        CHECK(toast(*r).empty());
        CHECK(r->host->stats().errors == 0);
        r.reset();
        std::error_code ec;
        sfs::remove_all(dir, ec);
    }
    {
        const auto dir = temp_dir("quicksave3d");
        auto r = start_template("3d", dir);
        r->frames(30);
        const auto dropped = [&] {
            int n = 0;
            r->scene.world().each<scene::Name>([&](scene::Entity, scene::Name& nm) { n += nm.value == "Dropped Crate"; });
            return n;
        };
        press(*r, "E");
        r->frames(30);
        CHECK(dropped() == 1);
        press(*r, "F5");
        press(*r, "E");
        press(*r, "E");
        CHECK(dropped() == 3);
        press(*r, "F9");
        CHECK(dropped() == 1);
        CHECK(toast(*r) == "Game loaded");
        // The player's list of dropped crates came back as entities: dropping 8 more removes the
        // restored one first (the cap is 8).
        for (int i = 0; i < 8; ++i) press(*r, "E");
        CHECK(dropped() == 8);
        CHECK(r->host->stats().errors == 0);
        r.reset();
        std::error_code ec;
        sfs::remove_all(dir, ec);
    }
}

TEST_CASE("project: save_version is saved and validated") {
    const auto dir = temp_dir("savever");
    auto p = project::Project::create(dir, "Saves", "simulation");
    REQUIRE(p.ok());
    CHECK(p->settings().save_version == 1);
    auto text = fs::read_text(dir / "victus.project");
    REQUIRE(text.ok());
    CHECK(text->find("save_version") == std::string::npos); // the default is not written
    p->settings().save_version = 4;
    REQUIRE(p->save().ok());
    auto q = project::Project::open(dir);
    REQUIRE(q.ok());
    CHECK(q->settings().save_version == 4);
    CHECK_FALSE(q->settings().extra.find("save_version"));
    CHECK(q->dev_save_directory() == q->root() / ".victus" / "saves");
    auto doc = json::parse(*fs::read_text(dir / "victus.project"));
    REQUIRE(doc.ok());
    doc->set("save_version", 0);
    REQUIRE(fs::write_text_atomic(dir / "victus.project", json::stringify(*doc)).ok());
    CHECK_FALSE(project::Project::open(dir).ok());
    std::error_code ec;
    sfs::remove_all(dir, ec);
}

TEST_CASE("project: input actions are saved, rebindable in the file and validated") {
    const auto dir = temp_dir("input");
    auto p = project::Project::create(dir, "Input", "2d");
    REQUIRE(p.ok());
    CHECK(p->settings().input == gameplay::default_input_map());
    auto& jump = p->settings().input.actions[static_cast<std::size_t>(
        p->settings().input.find("jump") - p->settings().input.actions.data())];
    jump.positive = {gameplay::parse_binding("K").value(), gameplay::parse_binding("Pad.North").value()};
    REQUIRE(p->save().ok());
    auto q = project::Project::open(dir);
    REQUIRE(q.ok());
    REQUIRE(q->settings().input.find("jump"));
    CHECK(q->settings().input.find("jump")->positive.size() == 2);
    CHECK(gameplay::binding_to_string(q->settings().input.find("jump")->positive[0]) == "K");
    CHECK_FALSE(q->settings().extra.find("input"));

    // The rebinding reaches the template's scripts: K jumps, Space no longer does.
    auto r = start_template("2d", dir, false);
    r->frames(30);
    r->input.set_key("Space", true);
    r->frames(10);
    r->input.set_key("Space", false);
    CHECK(r->pos("Player").y == doctest::Approx(-2.55f).epsilon(4e-3));
    r->input.set_key("K", true);
    r->frames(10);
    r->input.set_key("K", false);
    CHECK(r->pos("Player").y > -1.5f);
    r.reset();

    // A project without an "input" section gets the defaults; a broken one fails to open.
    auto text = fs::read_text(dir / "victus.project");
    REQUIRE(text.ok());
    auto doc = json::parse(*text);
    REQUIRE(doc.ok());
    json::Object without;
    for (const auto& [k, v] : doc->as_object())
        if (k != "input") without.emplace_back(k, v);
    REQUIRE(fs::write_text_atomic(dir / "victus.project", json::stringify(json::Value(without))).ok());
    auto d = project::Project::open(dir);
    REQUIRE(d.ok());
    CHECK(d->settings().input == gameplay::default_input_map());
    auto broken = json::parse(R"({"actions": [{"name": "jump", "bindings": ["Pad.Nope"]}]})");
    REQUIRE(broken.ok());
    doc->set("input", *broken);
    REQUIRE(fs::write_text_atomic(dir / "victus.project", json::stringify(*doc)).ok());
    auto e = project::Project::open(dir);
    REQUIRE_FALSE(e.ok());
    CHECK(e.error().message.find("Pad.Nope") != std::string::npos);
    std::error_code ec;
    sfs::remove_all(dir, ec);
}

TEST_CASE("templates: 3D companion follows the player over the navmesh") {
    const auto dir = temp_dir("tpl3dnav");
    auto r = start_template("3d", dir);
    r->frames(2);
    REQUIRE(r->navigation->valid()); // built on first use from the static colliders
    CHECK(r->navigation->stats().polygons > 0);
    const scene::Entity buddy = r->scene.find_by_name("Buddy");
    REQUIRE_FALSE(buddy.is_null());
    CHECK(r->navigation->agent_state(buddy).active);
    const math::Vec3 b0 = r->pos("Buddy");
    // Walk the player to the far right edge; the buddy re-plans and follows.
    r->input.set_key("D", true);
    r->frames(100);
    r->input.set_key("D", false);
    const math::Vec3 p = r->pos("Player");
    CHECK(p.x > 7.0f);
    CHECK(r->navigation->stats().moving == 1);
    r->frames(300);
    const math::Vec3 b1 = r->pos("Buddy");
    CAPTURE(b0);
    CAPTURE(b1);
    CAPTURE(p);
    CHECK(b1.x > b0.x + 6.0f);
    const float gap = std::hypot(b1.x - p.x, b1.z - p.z);
    CHECK(gap < 2.6f);
    CHECK(gap > 0.8f); // keeps its distance
    CHECK(std::fabs(b1.y) < 0.1f); // walked on the ground (character mover)
    CHECK(r->navigation->stats().moving == 0); // arrived and idles
    for (const scene::Entity c : r->scene.children(buddy)) {
        const auto anim = r->animation->status(r->scene, c);
        if (anim.valid) CHECK(anim.clip == "Idle");
    }
    CHECK(r->host->stats().errors == 0);
    for (const auto& e : r->host->recent_errors()) MESSAGE(e);
    std::error_code ec;
    sfs::remove_all(dir, ec);
}

TEST_CASE("templates: simulation camera orbits the field") {
    const auto dir = temp_dir("tplsim");
    auto r = start_template("simulation", dir);
    r->frames(1);
    const math::Vec3 c0 = r->pos("Main Camera");
    r->frames(120);
    const math::Vec3 c1 = r->pos("Main Camera");
    CHECK(math::length(c1 - c0) > 0.5f);
    CHECK(c1.y == doctest::Approx(c0.y));
    CHECK(r->host->stats().errors == 0);
    std::error_code ec;
    sfs::remove_all(dir, ec);
}

TEST_CASE("templates: multiplayer arena plays offline and over a network session") {
    const auto dir = temp_dir("tplmp");
    {
        // Offline: the template player is the player; walking right collects coins.
        auto r = start_template("multiplayer", dir);
        r->frames(30);
        const auto player = r->scene.find_by_name("Player");
        REQUIRE_FALSE(player.is_null());
        CHECK(net::get_var(r->scene, player, "score").as_number() == 0.0);
        CHECK(r->text("Net Status").find("Offline") != std::string::npos);
        r->input.set_key("D", true);
        r->frames(150);
        CHECK(r->pos("Player").x > 8.0f);
        CHECK(r->pos("Player").x < 11.7f); // stopped by the arena wall
        r->input.set_key("D", false);
        r->frames(20);
        CHECK(net::get_var(r->scene, player, "score").as_number() >= 1.0); // Coin 1 is on the way
        CHECK(r->text("Scoreboard Text").find("You") != std::string::npos);
        CHECK(r->host->stats().errors == 0);
    }
    {
        // Server + two clients on an in-memory network, all running the template's scripts.
        net::MemoryNetwork network;
        auto server_run = start_template("multiplayer", dir, false);
        net::ServerOptions so;
        so.game_id = "mp";
        net::NetServer server(std::move(network.listen(1)).value(), so);
        server.install(server_run->sim);
        std::vector<std::unique_ptr<TemplateRun>> runs;
        std::vector<std::unique_ptr<net::NetClient>> clients;
        for (const char* name : {"ann", "ben"}) {
            runs.push_back(start_template("multiplayer", dir, false));
            net::ClientOptions co;
            co.host = "memory";
            co.port = 1;
            co.name = name;
            co.game_id = "mp";
            clients.push_back(std::make_unique<net::NetClient>(network.endpoint(), co));
            clients.back()->install(runs.back()->sim);
        }
        auto step = [&](int n) {
            for (int i = 0; i < n; ++i) {
                server_run->frames(1);
                for (auto& r : runs) r->frames(1);
            }
        };
        step(60);
        REQUIRE(clients[0]->connected());
        REQUIRE(clients[1]->connected());
        CHECK(server.clients().size() == 2);
        // Ann (client 1, spawned at about (1.7, 0, 0.2) on the ring around the template) walks
        // diagonally through Coin 1 at (3.3, 0.7, 1.4); ben stands still.
        runs[0]->input.set_key("D", true);
        runs[0]->input.set_key("S", true);
        step(30);
        runs[0]->input.set_key("D", false);
        runs[0]->input.set_key("S", false);
        step(30);
        double ann = 0, ben = -1;
        for (const auto& c : clients[1]->clients()) { // as ben sees the scores
            REQUIRE_FALSE(c.player.is_null());
            const double v = net::get_var(runs[1]->scene, c.player, "score").as_number();
            (c.name == "ann" ? ann : ben) = v;
        }
        CHECK(ann >= 1.0);
        CHECK(ben == 0.0);
        // Ben's camera follows his own predicted player and his HUD shows the session.
        const auto ben_player = clients[1]->local_player();
        REQUIRE_FALSE(ben_player.is_null());
        const auto cam = runs[1]->pos("Main Camera");
        const auto bp = runs[1]->scene.world().get<scene::Transform>(ben_player)->position;
        CHECK(std::abs(cam.x - bp.x) < 1.0f);
        CHECK(runs[1]->text("Net Status").find("Online   player ") != std::string::npos);
        CHECK(runs[1]->text("Scoreboard Text").find("ann") != std::string::npos);
        CHECK(runs[1]->text("Scoreboard Text").find("(you)") != std::string::npos);
        CHECK(server_run->host->stats().errors == 0);
        for (auto& r : runs) CHECK(r->host->stats().errors == 0);
    }
    std::error_code ec;
    sfs::remove_all(dir, ec);
}

namespace {
// Runs `frames` ticks of `r`, feeding its ManualInput through a recorder (as the player does).
void record_session(TemplateRun& r, gameplay::Capture& cap, int frames, const std::function<void(int)>& script) {
    gameplay::ReplayInput fed;
    r.sim.input = &fed;
    for (int i = 0; i < frames; ++i) {
        script(i);
        const auto state = gameplay::sample_input(r.input);
        cap.add_frame(state, 1.0f / 60.0f);
        fed.set(state);
        r.sim.tick(r.scene, 1.0f / 60.0f);
        fed.end_frame();
        r.input.end_frame();
        cap.after_tick(r.scene);
    }
    cap.finish(r.scene);
    r.sim.input = &r.input;
}

int count_named(scene::Scene& s, std::string_view prefix) {
    int n = 0;
    s.world().each<scene::Name>([&](scene::Entity, scene::Name& nm) { n += nm.value.rfind(prefix, 0) == 0; });
    return n;
}
} // namespace

TEST_CASE("captures: a recorded 3D session replays exactly; a nudged replay reports the frame") {
    const auto dir = temp_dir("tplcap");
    gameplay::Capture cap;
    cap.seed = 0x5eed;
    cap.digest_interval = 30;
    int crates_recorded = 0;
    {
        auto r = start_template("3d", dir, true, cap.seed);
        record_session(*r, cap, 360, [&](int i) {
            r->input.set_key("W", i >= 10 && i < 120);
            r->input.set_key("E", i == 60 || i == 61 || i == 200); // drop crates (prefab instances)
            r->input.set_key("D", i >= 130 && i < 220);
            r->input.set_key("Space", i == 150);
            r->input.set_gamepad_axis(0, 0, i >= 240 && i < 300 ? -0.6f : 0.0f); // left stick
        });
        crates_recorded = count_named(r->scene, "Crate");
        CHECK(r->host->stats().errors == 0);
    }
    REQUIRE(cap.digests.size() == 12);
    // Through the file format.
    auto parsed = gameplay::Capture::parse(cap.to_text());
    REQUIRE(parsed.ok());
    CHECK(parsed->frames == cap.frames);
    CHECK(parsed->seed == cap.seed);

    auto replay_into = [&](TemplateRun& r, gameplay::CaptureReplay& rp, int nudge_at) {
        r.sim.input = &rp.input();
        while (!rp.done()) {
            const float dt = rp.begin_tick();
            r.sim.tick(r.scene, dt);
            if (static_cast<int>(rp.frame()) == nudge_at)
                r.scene.world().get<scene::Transform>(r.scene.find_by_name("Player"))->position.x += 0.01f;
            if (!rp.end_tick(r.scene)) break;
        }
    };
    {
        auto r = start_template("3d", dir, false, parsed->seed);
        gameplay::CaptureReplay rp(*parsed);
        replay_into(*r, rp, -1);
        CHECK_FALSE(rp.diverged());
        CHECK(rp.digests_checked() == 12);
        CHECK(count_named(r->scene, "Crate") == crates_recorded);
    }
    {
        auto r = start_template("3d", dir, false, parsed->seed);
        gameplay::CaptureReplay rp(*parsed);
        replay_into(*r, rp, 100);
        CHECK(rp.diverged());
        CHECK(rp.mismatch().rfind("frame 120:", 0) == 0);
        CHECK(rp.frame() == 120);
    }
}

TEST_CASE("captures: script random seeds make coin hops repeatable") {
    const auto dir = temp_dir("tplseed");
    auto coin_after_walk = [&](std::uint64_t seed, bool create) {
        auto r = start_template("multiplayer", dir, create, seed);
        r->input.set_key("D", true);
        r->frames(170); // collects Coin 1, which hops to a random free spot
        CHECK(net::get_var(r->scene, r->scene.find_by_name("Player"), "score").as_number() >= 1.0);
        return r->pos("Coin 1");
    };
    const math::Vec3 a = coin_after_walk(7, true);
    const math::Vec3 b = coin_after_walk(7, false);
    const math::Vec3 c = coin_after_walk(8, false);
    CHECK(math::length(a - b) < 1e-5f);
    CHECK(math::length(a - c) > 0.1f);
}

TEST_CASE("project: bridges section resolves helper process launches") {
    const auto dir = temp_dir("bridges");
    auto p = project::Project::create(dir, "Bridges", "3d");
    REQUIRE(p.ok());
    auto doc = json::parse(R"({"sim": {"command": ["${env:VICTUS_TEST_BRIDGE_PROGRAM:-tools/sim}", "serve", "--saves", "${saves}/sim",
                                                  "${project}", "${env:VICTUS_TEST_UNSET_VAR:-fallback}"],
                                      "working_directory": "data", "discard_stderr": true},
                              "bare": {"command": ["dotnet", "x.dll"]},
                              "broken": {"command": ["${nope}"]},
                              "unset": {"command": ["${env:VICTUS_TEST_UNSET_VAR}"]},
                              "empty": {"command": []}})");
    REQUIRE(doc.ok());
    p->settings().extra.set("bridges", std::move(*doc));
    const sfs::path saves = dir / ".victus" / "saves";
    auto sim = project::bridge_launch(*p, "sim", saves);
    REQUIRE(sim.ok());
    REQUIRE(sim->args.size() == 6);
    CHECK(sfs::path(sim->args[0]) == (p->root() / "tools" / "sim").lexically_normal());
    CHECK(sim->args[3] == saves.generic_string() + "/sim");
    CHECK(sim->args[4] == p->root().generic_string());
    CHECK(sim->args[5] == "fallback");
    CHECK(sfs::path(sim->working_directory) == (p->root() / "data").lexically_normal());
    CHECK(sim->discard_stderr);
    CHECK(sfs::is_directory(saves)); // created for the helper
    auto bare = project::bridge_launch(*p, "bare", saves);
    REQUIRE(bare.ok());
    CHECK(bare->args[0] == "dotnet"); // looked up on PATH
    CHECK(sfs::path(bare->working_directory) == p->root());
    CHECK_FALSE(project::bridge_launch(*p, "broken", saves));
    CHECK_FALSE(project::bridge_launch(*p, "unset", saves));
    CHECK_FALSE(project::bridge_launch(*p, "empty", saves));
    auto missing = project::bridge_launch(*p, "other", saves);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().message.find("no bridge named 'other'") != std::string::npos);
    sfs::remove_all(dir);
}
