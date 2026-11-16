// SPDX-License-Identifier: Apache-2.0
// Regression capture tests (Slice 24): input sampling and replay edges, the .vcapture format
// (change-only frames, validation), state digests and seeded script randomness.
#include "victus/gameplay/capture.hpp"
#include "victus/physics/physics.hpp"
#include "victus/script/script.hpp"

#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace victus;
using gameplay::Capture;
using gameplay::InputState;

TEST_CASE("capture: sampled input replays with the same edges") {
    gameplay::ManualInput in;
    gameplay::ReplayInput out;
    in.set_key("D", true);
    in.set_mouse(1, true);
    in.set_mouse_position({10, 20});
    in.set_gamepad_axis(1, 2, -0.25f);
    in.set_gamepad_button(1, 3, true);
    auto s = gameplay::sample_input(in);
    CHECK(s.keys == std::vector<std::string>{"D"});
    CHECK(s.mouse == 2);
    CHECK(s.pads[1].connected);
    CHECK_FALSE(s.pads[0].connected);
    out.set(s);
    CHECK(out.key_down("D"));
    CHECK(out.key_pressed("D"));
    CHECK(out.mouse_pressed(1));
    CHECK(out.mouse_position().x == 10);
    CHECK(out.mouse_delta().y == 20); // from the origin, as ManualInput reports it
    CHECK(out.gamepad_axis(1, 2) == -0.25f);
    CHECK(out.gamepad_button(1, 3));
    out.end_frame();
    in.end_frame();
    in.set_key("D", false);
    out.set(gameplay::sample_input(in));
    CHECK(out.key_released("D"));
    CHECK_FALSE(out.key_down("D"));
    CHECK(out.gamepad_button(1, 3, true)); // previous frame
}

TEST_CASE("capture: file format round-trips change-only frames and rejects bad files") {
    Capture c;
    c.project = "Demo";
    c.scene = "scenes/main.vscene";
    c.seed = 0xfedcba9876543210ull; // needs all 64 bits (JSON numbers would round it)
    InputState idle, walk, jump;
    walk.keys = {"D"};
    jump.keys = {"D", "Space"};
    jump.pads[0].connected = true;
    jump.pads[0].axes[0] = 0.123456f;
    jump.pads[0].buttons = 5;
    jump.mouse_position = {1.5f, 2.25f};
    for (const auto& f : {idle, walk, walk, jump, walk, idle}) c.add_frame(f, 1.0f / 60.0f);
    c.frame_dt[5] = 0.0213f; // variable frame time (windowed play)
    c.digests.push_back({6, "0123456789abcdef"});
    const std::string text = c.to_text();
    CHECK(text.find("\"+\":[\"Space\"]") != std::string::npos); // only changes are stored
    auto p = Capture::parse(text);
    REQUIRE(p.ok());
    CHECK(p->frames == c.frames);
    CHECK(p->frame_dt == c.frame_dt);
    CHECK(p->seed == c.seed);
    CHECK(p->project == "Demo");
    REQUIRE(p->digests.size() == 1);
    CHECK(p->digests[0].frame == 6);
    CHECK(p->digests[0].value == "0123456789abcdef");

    const auto dir = std::filesystem::temp_directory_path() / "victus_capture_tests";
    std::filesystem::create_directories(dir);
    REQUIRE(c.save(dir / "a.vcapture").ok());
    auto loaded = Capture::load(dir / "a.vcapture");
    REQUIRE(loaded.ok());
    CHECK(loaded->frames == c.frames);

    CHECK_FALSE(Capture::parse("{}").ok());
    CHECK_FALSE(Capture::parse(R"({"format":"victus.capture","version":99,"frames":[]})").ok());
    CHECK_FALSE(Capture::parse(R"({"format":"victus.capture","version":1,"frames":[{"+":["Nope"]}]})").ok());
    CHECK_FALSE(Capture::parse(R"({"format":"victus.capture","version":1,"frames":[{"dt":-1}]})").ok());
    CHECK_FALSE(Capture::parse(R"({"format":"victus.capture","version":1,"seed":"xyz","frames":[]})").ok());
    CHECK(Capture::parse(R"({"format":"victus.capture","version":1,"seed":"1","frames":[]})").ok());
}

TEST_CASE("capture: state digests ignore creation order but see names and movement") {
    auto build = [](bool reversed) {
        auto s = std::make_unique<scene::Scene>();
        std::vector<std::string> names = {"A", "B", "C"};
        if (reversed) std::reverse(names.begin(), names.end());
        for (const auto& n : names) {
            const auto e = s->create(n);
            s->world().get<scene::Transform>(e)->position = {static_cast<float>(n[0] - 'A'), 0, 0};
            s->create("Child", e);
        }
        return s;
    };
    auto a = build(false);
    auto b = build(true);
    CHECK(gameplay::state_digest(*a) == gameplay::state_digest(*b));
    a->world().get<scene::Transform>(a->find_by_name("B"))->position.y += 0.001f;
    CHECK(gameplay::state_digest(*a) != gameplay::state_digest(*b));
    a->world().get<scene::Transform>(a->find_by_name("B"))->position.y -= 0.001f;
    CHECK(gameplay::state_digest(*a) == gameplay::state_digest(*b));
    a->world().get<scene::Name>(a->find_by_name("C"))->value = "D";
    CHECK(gameplay::state_digest(*a) != gameplay::state_digest(*b));
}

TEST_CASE("capture: CaptureReplay checks digests at their frames") {
    scene::Scene s;
    const auto e = s.create("Mover");
    Capture c;
    c.digest_interval = 2;
    for (int i = 0; i < 5; ++i) {
        c.add_frame({}, 1.0f / 60.0f);
        s.world().get<scene::Transform>(e)->position.x += 1;
        c.after_tick(s);
    }
    c.finish(s);
    REQUIRE(c.digests.size() == 3); // frames 2, 4 and the final 5
    scene::Scene t;
    const auto f = t.create("Mover");
    gameplay::CaptureReplay rp(c);
    int ticks = 0;
    while (!rp.done()) {
        (void)rp.begin_tick();
        t.world().get<scene::Transform>(f)->position.x += ticks == 3 ? 2.0f : 1.0f; // diverges on frame 4
        ++ticks;
        if (!rp.end_tick(t)) break;
    }
    CHECK(rp.diverged());
    CHECK(rp.frame() == 4);
    CHECK(rp.digests_checked() == 2);
    // The recorded per-entity state names the entity that diverged.
    MESSAGE(rp.mismatch());
    CHECK(rp.mismatch() == "frame 4: Mover: position (5, 0, 0) expected (4, 0, 0)");
}

TEST_CASE("capture: divergence reports name changed, missing and extra entities") {
    scene::Scene a;
    const auto p = a.create("Player");
    a.create("Coin");
    a.create("Coin");
    const auto want = gameplay::state_lines(a);
    CHECK(want.front() == "#entities 3");
    CHECK(gameplay::describe_difference(want, want) == "state differs");
    a.world().get<scene::Transform>(p)->rotation = math::Quat::axis_angle({0, 1, 0}, 1.57079633f);
    a.destroy(a.find_by_name("Coin"));
    a.create("Ghost");
    const auto got = gameplay::state_lines(a);
    const auto text = gameplay::describe_difference(want, got);
    MESSAGE(text);
    CHECK(text.find("Coin: 1 present, expected 2") != std::string::npos);
    CHECK(text.find("Ghost: not in the recording") != std::string::npos);
    CHECK(text.find("Player: rotation (0, 0.7071, 0, 0.7071) expected (0, 0, 0, 1)") != std::string::npos);
    CHECK(gameplay::describe_difference(want, got, 1).find("; 2 more") != std::string::npos);
    // Captures keep the state through save/parse, and detail can be turned off.
    Capture c;
    c.digest_interval = 1;
    c.add_frame({}, 0.01f);
    c.after_tick(a);
    auto back = Capture::parse(c.to_text());
    REQUIRE(back);
    REQUIRE(back.value().digests.size() == 1);
    CHECK(back.value().digests[0].state == got);
    Capture lean;
    lean.detail_limit = 0;
    lean.add_frame({}, 0.01f);
    lean.finish(a);
    CHECK(lean.digests.at(0).state.empty());
    CHECK(lean.to_text().find("\"state\"") == std::string::npos);
}

TEST_CASE("capture: a script random seed repeats math.random") {
    const auto dir = std::filesystem::temp_directory_path() / "victus_capture_seed";
    std::filesystem::create_directories(dir / "scripts");
    std::ofstream(dir / "scripts" / "roll.luau") << R"lua(
local R = {}
function R:start() self.entity.name = string.format("%.6f", math.random()) end
return R
)lua";
    auto roll = [&](std::uint64_t seed) {
        script::HostOptions o;
        o.assets_root = dir;
        o.random_seed = seed;
        auto host = script::ScriptHost::create(o);
        REQUIRE(host.ok());
        scene::Scene s;
        const auto e = s.create("Roller");
        s.world().add<script::ScriptComponent>(e).script = "scripts/roll.luau";
        (*host)->update(s, {}, nullptr);
        return s.world().get<scene::Name>(e)->value;
    };
    CHECK(roll(42) == roll(42));
    CHECK(roll(42) != roll(43));
}

TEST_CASE("capture: state probes add script fields and physics velocities to digests") {
    const auto dir = std::filesystem::temp_directory_path() / "victus_capture_probes";
    std::filesystem::create_directories(dir / "scripts");
    // `score` never moves anything: only the script probe can see it diverge.
    std::ofstream(dir / "scripts" / "keeper.luau") << R"lua(
local K = {}
function K:start() self.score = 0; self.label = "keeper"; self.flag = true; self._hidden = 7; self.aim = vector.create(1, 2, 3) end
function K:update(dt) self.score += if input.down("Space") then 2 else 1 end
return K
)lua";
    script::register_components();
    auto run = [&](int frames, Capture* record, gameplay::CaptureReplay* replay) {
        script::HostOptions o;
        o.assets_root = dir;
        auto host = script::ScriptHost::create(o);
        REQUIRE(host.ok());
        auto world = physics::PhysicsWorld::create();
        REQUIRE(world.ok());
        scene::Scene s;
        const auto keeper = s.create("Keeper");
        s.world().add<script::ScriptComponent>(keeper).script = "scripts/keeper.luau";
        const auto box = s.create("Box");
        s.world().get<scene::Transform>(box)->position = {0, 10, 0};
        s.world().add<physics::RigidBodyComponent>(box);
        s.world().add<physics::ColliderComponent>(box);
        gameplay::Simulation sim(1.0f / 60.0f);
        script::add_script_systems(sim, **host);
        physics::add_physics_systems(sim, **world);
        if (replay) sim.input = &replay->input();
        for (int i = 0; i < frames; ++i) {
            if (replay) (void)replay->begin_tick();
            if (record) record->add_frame({}, 1.0f / 60.0f);
            sim.tick(s, 1.0f / 60.0f);
            if (record) record->after_tick(s);
            if (replay && !replay->end_tick(s)) break;
        }
        if (record) record->finish(s);
        return gameplay::state_lines(s);
    };
    Capture c;
    c.digest_interval = 5;
    const auto lines = run(10, &c, nullptr);
    auto has = [&](std::string_view l) { return std::find(lines.begin(), lines.end(), l) != lines.end(); };
    CHECK(has("@Keeper|script.score|10"));
    CHECK(has("@Keeper|script.label|\"keeper\""));
    CHECK(has("@Keeper|script.flag|true"));
    CHECK(has("@Keeper|script.aim|(1, 2, 3)"));
    CHECK(std::none_of(lines.begin(), lines.end(), [](const std::string& l) { return l.find("_hidden") != std::string::npos; }));
    const auto vel = std::find_if(lines.begin(), lines.end(), [](const std::string& l) { return l.rfind("@Box|physics.velocity|(0, -", 0) == 0; });
    CHECK(vel != lines.end()); // falling
    CHECK(has("@Box|physics.spin|(0, 0, 0)"));
    // Scenes without engine bindings get no probe lines.
    {
        scene::Scene empty;
        CHECK(gameplay::state_lines(empty, false) == std::vector<std::string>{"#entities 0"});
    }

    // Same run: replays clean. A script-only divergence is named by its field.
    {
        gameplay::CaptureReplay rp(c);
        (void)run(10, nullptr, &rp);
        CHECK_FALSE(rp.diverged());
        CHECK(rp.digests_checked() == 2);
    }
    {
        Capture held = c;
        held.frames[7].keys = {"Space"}; // the script scores double on frame 8; nothing moves
        gameplay::CaptureReplay rp(held);
        (void)run(10, nullptr, &rp);
        REQUIRE(rp.diverged());
        MESSAGE(rp.mismatch());
        CHECK(rp.mismatch() == "frame 10: Keeper: script.score 11 expected 10");
    }
    // A version-1 capture ignores probes: the same divergence goes unseen, as it did before.
    {
        auto v1 = Capture::parse(c.to_text());
        REQUIRE(v1.ok());
        CHECK(v1->version == 2);
        Capture old;
        old.version = 1;
        old.digest_interval = 5;
        (void)run(10, &old, nullptr);
        auto text = old.to_text();
        CHECK(text.find("\"version\":1") != std::string::npos);
        auto back = Capture::parse(text);
        REQUIRE(back.ok());
        CHECK(back->version == 1);
        back->frames[7].keys = {"Space"};
        gameplay::CaptureReplay rp(*back);
        (void)run(10, nullptr, &rp);
        CHECK_FALSE(rp.diverged());
    }
}

TEST_CASE("capture: probe lines and entity names that start with '@' never collide") {
    scene::Scene a;
    a.create("@Odd");
    const auto lines = gameplay::state_lines(a);
    REQUIRE(lines.size() == 2);
    CHECK(lines[1].rfind("\\@Odd|", 0) == 0);
    CHECK(gameplay::state_lines(a, false)[1].rfind("@Odd|", 0) == 0); // version 1 unchanged
    scene::Scene b;
    const auto e = b.create("@Odd");
    b.world().get<scene::Transform>(e)->position.x = 2;
    CHECK(gameplay::describe_difference(lines, gameplay::state_lines(b)) == "@Odd: position (2, 0, 0) expected (0, 0, 0)");
    std::vector<std::string> want = {"#entities 1", "@P|script.hp|3"}, got = {"#entities 1", "@P|script.hp|2"};
    CHECK(gameplay::describe_difference(want, got) == "P: script.hp 2 expected 3");
    CHECK(gameplay::describe_difference(want, {"#entities 1"}) == "P: script.hp missing, expected 3");
    CHECK(gameplay::describe_difference({"#entities 1"}, got) == "P: script.hp 2 not in the recording");
    CHECK(gameplay::probe_number(-0.00001) == "0");
    CHECK(gameplay::probe_number(1.23456) == "1.2346");
    CHECK(gameplay::probe_vector(math::Vec2{1, -2}) == "(1, -2)");
}
