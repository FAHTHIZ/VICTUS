// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/gameplay/gameplay.hpp"
#include "victus/project/project.hpp"
#include "victus/audio/audio.hpp"
#include "victus/physics/physics.hpp"
#include "victus/script/script.hpp"

#include <filesystem>

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
    REQUIRE(p->save().ok());
    auto q = project::Project::open(dir);
    REQUIRE(q.ok());
    CHECK(q->settings().render_quality == "low");
    CHECK(q->settings().render_scale == 0.5f);
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
};

std::unique_ptr<TemplateRun> start_template(const char* id, const sfs::path& dir) {
    project::register_components();
    auto p = project::Project::create(dir, "Scripted", id);
    REQUIRE(p.ok());
    auto r = std::make_unique<TemplateRun>();
    auto path = p->startup_scene_path();
    REQUIRE(path.ok());
    REQUIRE(r->scene.load_file(*path).ok());
    script::HostOptions o;
    o.assets_root = dir / "assets";
    auto h = script::ScriptHost::create(o);
    REQUIRE(h.ok());
    r->host = std::move(h).value();
    gameplay::add_builtin_systems(r->sim);
    script::add_script_systems(r->sim, *r->host);
    auto w = physics::PhysicsWorld::create();
    REQUIRE(w.ok());
    r->physics = std::move(w).value();
    physics::add_physics_systems(r->sim, *r->physics);
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
    CHECK(r->host->stats().instances == 4); // player + 3 coins
    CHECK(r->audio->is_playing(r->scene.find_by_name("Music"))); // looping streamed music
    CHECK(r->audio->stats().one_shots == 0);
    // Run right: the player stops at the right wall and collects the coin on the ground.
    r->input.set_key("D", true);
    r->frames(300);
    CHECK(r->pos("Player").x == doctest::Approx(8.8f - 0.45f).epsilon(2e-3));
    CHECK(r->pos("Player").y == doctest::Approx(-2.55f).epsilon(4e-3));
    CHECK(r->scene.find_by_name("Coin 3").is_null());
    CHECK_FALSE(r->scene.find_by_name("Coin 1").is_null());
    CHECK(r->audio->stats().one_shots == 1); // coin pickup sound
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
    CHECK(r->host->stats().errors == 0);
    for (const auto& e : r->host->recent_errors()) MESSAGE(e);
    std::error_code ec;
    sfs::remove_all(dir, ec);
}

TEST_CASE("templates: 3D character climbs the steps, jumps and swings the gate") {
    const auto dir = temp_dir("tpl3d_char");
    auto r = start_template("3d", dir);
    r->frames(5);
    const scene::Entity player = r->scene.find_by_name("Player");
    const scene::Entity gate = r->scene.find_by_name("Gate");
    REQUIRE(r->physics->character_state(player).grounded);
    REQUIRE(r->physics->joint_state(gate).active);
    // Jump in place.
    r->input.set_key("Space", true);
    r->frames(1);
    r->input.set_key("Space", false);
    float apex = 0;
    for (int i = 0; i < 40; ++i) {
        r->frames(1);
        apex = std::max(apex, r->pos("Player").y);
    }
    CHECK(apex > 1.5f); // 6 m/s -> 1.83 m
    r->frames(50);
    CHECK(r->physics->character_state(player).grounded);
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
    r->frames(240);
    CHECK(r->pos("Player").z == doctest::Approx(-9.0f).epsilon(0.02)); // stops at the bounds
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
