// SPDX-License-Identifier: Apache-2.0
// Regression capture tests (Slice 24): input sampling and replay edges, the .vcapture format
// (change-only frames, validation), state digests and seeded script randomness.
#include "victus/gameplay/capture.hpp"
#include "victus/script/script.hpp"

#include <doctest.h>

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
