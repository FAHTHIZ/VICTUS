// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/gameplay/gameplay.hpp"
#include "victus/project/project.hpp"

#include <filesystem>

using namespace victus;
namespace sfs = std::filesystem;

namespace {
sfs::path temp_dir(const char* tag) {
    return sfs::temp_directory_path() / (std::string("victus_") + tag + "_" + Uuid::generate().to_string().substr(0, 8));
}
} // namespace

TEST_CASE("gameplay: fixed and variable phases, built-in behaviours") {
    gameplay::register_components();
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
    gameplay::register_components();
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
