// SPDX-License-Identifier: Apache-2.0
// Animation controller (.vanim) tests: parsing/validation, blend-space weights, the state
// machine (conditions, triggers, exit time, any-state, cross-fades) and the AnimationSystem API.
#include "victus/animation/animation.hpp"
#include "victus/core/json.hpp"

#include <doctest.h>

#include <cmath>
#include <map>
#include <numeric>

using namespace victus;
using namespace victus::animation;
using math::Vec2;
using scene::Entity;
using scene::Scene;

namespace {

bool near(float a, float b, float eps = 1e-4f) { return std::abs(a - b) <= eps; }

// A clip holding Hips.x at a constant value, so a blended pose reads back as sum(w_i * x_i).
Clip constant_clip(std::string name, float duration, float x) {
    Clip c{.name = std::move(name), .duration = duration};
    Channel ch{.node = 0, .path = Path::Translation, .interpolation = Interpolation::Linear, .times = {0, duration}};
    ch.values = {x, 0, 0, x, 0, 0};
    c.channels.push_back(std::move(ch));
    return c;
}

std::shared_ptr<AnimationSet> make_set() {
    auto set = std::make_shared<AnimationSet>();
    set->nodes.push_back({.name = "Hips", .parent = -1, .rest = {.translation = {-1, 0, 0}}});
    set->clips.push_back(constant_clip("Idle", 1.0f, 0.0f));
    set->clips.push_back(constant_clip("Walk", 2.0f, 1.0f));
    set->clips.push_back(constant_clip("Run", 1.0f, 3.0f));
    set->clips.push_back(constant_clip("Jump", 0.5f, 10.0f));
    return set;
}

constexpr const char* kController = R"({
  "version": 1,
  "parameters": [
    { "name": "speed", "type": "float" },
    { "name": "grounded", "type": "bool", "default": true },
    { "name": "jump", "type": "trigger" },
    { "name": "stance", "type": "int", "default": 0 },
    { "name": "dead", "type": "bool" }
  ],
  "default_state": "Locomotion",
  "states": [
    { "name": "Locomotion", "blend1d": { "parameter": "speed",
        "points": [ { "clip": "Walk", "at": 1.5 }, { "clip": "Idle", "at": 0 }, { "clip": "Run", "at": 4 } ] } },
    { "name": "Jump", "clip": "Jump", "loop": false },
    { "name": "Fall", "clip": "Idle" },
    { "name": "Dead", "clip": "Idle", "loop": false }
  ],
  "transitions": [
    { "from": "*", "to": "Dead", "duration": 0, "conditions": [ { "parameter": "dead", "op": "true" } ] },
    { "from": "Locomotion", "to": "Jump", "duration": 0, "conditions": [ { "parameter": "jump" } ] },
    { "from": "Jump", "to": "Fall", "duration": 0, "exit_time": 1 },
    { "from": "Fall", "to": "Locomotion", "duration": 0.5,
      "conditions": [ { "parameter": "grounded", "op": "true" }, { "parameter": "stance", "op": ">=", "value": 0 } ] }
  ]
})";

struct Rig {
    Scene scene;
    Entity root, hips;
    Uuid model = Uuid::generate();
    std::map<std::string, std::string> files;
    int loads = 0;
    AnimationSystem system{AnimationSettings{.controllers = [this](std::string_view p) -> Result<std::string> {
        ++loads;
        const auto it = files.find(std::string(p));
        if (it == files.end()) return make_error(ErrorCode::NotFound, "no such file");
        return it->second;
    }}};
    explicit Rig(const char* doc = kController) {
        files["anim/player.vanim"] = doc;
        system.add_set(model, make_set());
        root = scene.create("Character");
        hips = scene.create("Hips", root);
        auto& a = scene.world().add<AnimatorComponent>(root);
        a.model = model;
        a.controller = "anim/player.vanim";
        a.crossfade = 0;
    }
    float x() { return scene.world().get<scene::Transform>(hips)->position.x; }
    AnimatorComponent& a() { return *scene.world().get<AnimatorComponent>(root); }
    std::string state() { return system.status(scene, root).state; }
};

std::string parse_error(const std::string& doc) {
    auto c = Controller::parse(doc, "t.vanim");
    return c ? std::string() : c.error().message;
}

} // namespace

TEST_CASE("controller parses, sorts 1D points and round-trips through JSON") {
    auto c = Controller::parse(kController, "player.vanim");
    REQUIRE(c);
    const Controller& ctrl = **c;
    CHECK(ctrl.name == "player.vanim");
    REQUIRE(ctrl.states.size() == 4);
    CHECK(ctrl.default_state == 0);
    REQUIRE(ctrl.states[0].points.size() == 3);
    CHECK(ctrl.states[0].points[0].clip == "Idle"); // sorted by threshold
    CHECK(ctrl.states[0].points[2].clip == "Run");
    CHECK(ctrl.parameters[1].default_value == 1.0f);
    CHECK(ctrl.transitions[0].from == -1);
    CHECK(ctrl.transitions[1].conditions[0].op == ConditionOp::Trigger);
    CHECK(ctrl.transitions[2].exit_time == 1.0f);
    CHECK(ctrl.clip_names() == std::vector<std::string>{"Idle", "Walk", "Run", "Jump"});

    auto again = Controller::from_json(ctrl.to_json(), "player.vanim");
    REQUIRE(again);
    CHECK((*again)->parameters == ctrl.parameters);
    CHECK((*again)->states == ctrl.states);
    CHECK((*again)->transitions == ctrl.transitions);
    CHECK((*again)->default_state == ctrl.default_state);
    CHECK(json::stringify((*again)->to_json()) == json::stringify(ctrl.to_json()));
}

TEST_CASE("controller validation names the offending element") {
    const auto bad = [](const char* body) { return parse_error(body); };
    CHECK(bad(R"({"states": []})").find("at least one state") != std::string::npos);
    CHECK(bad(R"({"version": 9, "states": [{"name": "A", "clip": "x"}]})").find("newer") != std::string::npos);
    CHECK(bad(R"({"states": [{"name": "A"}]})").find("exactly one of") != std::string::npos);
    CHECK(bad(R"({"states": [{"name": "A", "clip": "x"}, {"name": "A", "clip": "y"}]})").find("duplicate state") !=
          std::string::npos);
    CHECK(bad(R"({"states": [{"name": "A", "blend1d": {"parameter": "nope", "points": [{"clip": "x", "at": 0}]}}]})")
              .find("unknown parameter \"nope\"") != std::string::npos);
    CHECK(bad(R"({"parameters": [{"name": "b", "type": "bool"}],
                  "states": [{"name": "A", "blend1d": {"parameter": "b", "points": [{"clip": "x", "at": 0}]}}]})")
              .find("must be a float or int") != std::string::npos);
    CHECK(bad(R"({"parameters": [{"name": "s"}],
                  "states": [{"name": "A", "blend1d": {"parameter": "s", "points": [{"clip": "x", "at": 1}, {"clip": "y", "at": 1}]}}]})")
              .find("share the threshold") != std::string::npos);
    CHECK(bad(R"({"parameters": [{"name": "s"}],
                  "states": [{"name": "A", "blend2d": {"x": "s", "y": "s", "points": [{"clip": "x", "at": 1}]}}]})")
              .find("[x, y]") != std::string::npos);
    CHECK(bad(R"({"states": [{"name": "A", "clip": "x"}], "transitions": [{"from": "A", "to": "A"}]})")
              .find("needs an \"exit_time\"") != std::string::npos);
    CHECK(bad(R"({"states": [{"name": "A", "clip": "x"}], "transitions": [{"from": "B", "to": "A", "exit_time": 1}]})")
              .find("transitions[0]: unknown state \"B\"") != std::string::npos);
    CHECK(bad(R"({"states": [{"name": "A", "clip": "x"}], "transitions": [{"from": "*", "to": "A", "exit_time": 1}]})")
              .find("any-state") != std::string::npos);
    CHECK(bad(R"({"parameters": [{"name": "s"}], "states": [{"name": "A", "clip": "x"}],
                  "transitions": [{"from": "A", "to": "A", "conditions": [{"parameter": "s"}]}]})")
              .find("need an \"op\"") != std::string::npos);
    CHECK(bad(R"({"parameters": [{"name": "t", "type": "trigger"}], "states": [{"name": "A", "clip": "x"}],
                  "transitions": [{"from": "A", "to": "A", "conditions": [{"parameter": "t", "op": ">"}]}]})")
              .find("only support op \"trigger\"") != std::string::npos);
    CHECK(bad(R"({"states": [{"name": "A", "clip": "x", "loop": false}],
                  "transitions": [{"from": "A", "to": "A", "exit_time": 2}]})")
              .find("never fires") != std::string::npos);
    CHECK(bad(R"({"parameters": [{"name": "p", "type": "vec3"}], "states": [{"name": "A", "clip": "x"}]})")
              .find("unknown parameter type") != std::string::npos);
    CHECK(bad(R"({"states": [{"name": "A", "clip": "x", "speed": -1}]})").find("\"speed\" must be >= 0") !=
          std::string::npos);
    CHECK(bad("[1,2").find("animation controller") != std::string::npos);
    CHECK(bad(R"({"default_state": "Z", "states": [{"name": "A", "clip": "x"}]})").find("unknown default_state") !=
          std::string::npos);
}

TEST_CASE("1D blend weights are piecewise linear and clamp at the ends") {
    const std::vector<float> t{0, 1.5f, 4};
    std::vector<float> w(3);
    blend_weights_1d(t, -1, w);
    CHECK(w == std::vector<float>{1, 0, 0});
    blend_weights_1d(t, 0.75f, w);
    CHECK(near(w[0], 0.5f));
    CHECK(near(w[1], 0.5f));
    blend_weights_1d(t, 4.0f, w);
    CHECK(w == std::vector<float>{0, 0, 1});
    blend_weights_1d(t, NAN, w);
    CHECK(w[0] == 1.0f);
}

TEST_CASE("2D gradient-band weights are exact at points, smooth and normalized") {
    const std::vector<Vec2> pts{{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    std::vector<float> w(pts.size());
    for (std::size_t i = 0; i < pts.size(); ++i) {
        blend_weights_2d(pts, pts[i], w);
        CHECK(near(w[i], 1.0f));
        CHECK(near(std::accumulate(w.begin(), w.end(), 0.0f), 1.0f));
    }
    // Halfway between centre and forward: those two share the weight, the rest stay out.
    blend_weights_2d(pts, {0, 0.5f}, w);
    CHECK(near(w[0], 0.5f));
    CHECK(near(w[3], 0.5f));
    CHECK(near(w[1] + w[2] + w[4], 0.0f));
    // Diagonal: right and forward contribute equally; sum is 1 anywhere, even outside the hull.
    blend_weights_2d(pts, {0.5f, 0.5f}, w);
    CHECK(near(w[1], w[3]));
    CHECK(w[1] > 0);
    blend_weights_2d(pts, {5, 5}, w);
    CHECK(near(std::accumulate(w.begin(), w.end(), 0.0f), 1.0f));
    // A single point always has the full weight; coincident points split it.
    std::vector<float> one(1);
    blend_weights_2d(std::vector<Vec2>{{2, 2}}, {9, -9}, one);
    CHECK(one[0] == 1.0f);
    std::vector<float> two(2);
    blend_weights_2d(std::vector<Vec2>{{1, 1}, {1, 1}}, {0, 0}, two);
    CHECK(near(two[0], 0.5f));
    CHECK(near(two[1], 0.5f));
}

TEST_CASE("1D blend space drives the pose from a float parameter, phase-synced") {
    Rig r;
    r.system.update(r.scene, 0);
    CHECK(r.state() == "Locomotion");
    CHECK(r.a().clip == "Locomotion"); // the clip field mirrors the state
    CHECK(near(r.x(), 0.0f));          // speed 0 -> Idle
    REQUIRE(r.system.set_parameter(r.scene, r.root, "speed", 0.75f));
    r.system.update(r.scene, 0);
    CHECK(near(r.x(), 0.5f)); // half Idle (0), half Walk (1)
    // Effective cycle = 0.5 * 1 s + 0.5 * 2 s.
    CHECK(near(r.system.status(r.scene, r.root).duration, 1.5f));
    r.system.update(r.scene, 0.75f);
    CHECK(near(r.system.status(r.scene, r.root).normalized_time, 0.5f));
    r.system.set_parameter(r.scene, r.root, "speed", 9.0f);
    r.system.update(r.scene, 0);
    CHECK(near(r.x(), 3.0f)); // clamped to Run
    const ControllerStatus cs = r.system.controller_status(r.scene, r.root);
    CHECK(cs.valid);
    REQUIRE(cs.weights.size() == 3);
    CHECK(cs.weights[2].clip == "Run");
    CHECK(near(cs.weights[2].weight, 1.0f));
    CHECK(r.system.stats().controlled == 1);
}

TEST_CASE("triggers fire once, exit time advances, conditions gate and fades blend") {
    Rig r;
    r.system.update(r.scene, 0);
    CHECK(r.system.set_trigger(r.scene, r.root, "jump"));
    CHECK(*r.system.parameter(r.scene, r.root, "jump") == 1.0f);
    r.system.update(r.scene, 0.01f);
    CHECK(r.state() == "Jump");
    CHECK(*r.system.parameter(r.scene, r.root, "jump") == 0.0f); // consumed
    CHECK(near(r.x(), 10.0f));
    CHECK(r.system.stats().transitions == 1);
    // Jump is 0.5 s, non-looping; exit_time 1 moves to Fall when it ends.
    r.system.update(r.scene, 0.3f);
    CHECK(r.state() == "Jump");
    r.system.update(r.scene, 0.3f);
    CHECK(r.state() == "Fall");
    // Fall -> Locomotion needs grounded && stance >= 0, with a 0.5 s fade.
    r.system.set_parameter(r.scene, r.root, "grounded", 0);
    r.system.set_parameter(r.scene, r.root, "stance", -2.6f);
    CHECK(*r.system.parameter(r.scene, r.root, "stance") == -3.0f); // ints round
    r.system.update(r.scene, 0.1f);
    CHECK(r.state() == "Fall");
    r.system.set_parameter(r.scene, r.root, "grounded", 1);
    r.system.set_parameter(r.scene, r.root, "stance", 1);
    r.system.set_parameter(r.scene, r.root, "speed", 4);
    r.system.update(r.scene, 0.0f); // fires; fade progress 0 -> still the Fall (Idle, x 0) pose
    CHECK(r.state() == "Locomotion");
    CHECK(near(r.x(), 0.0f));
    r.system.update(r.scene, 0.25f);
    CHECK(near(r.x(), 1.5f)); // halfway from Idle (0) to Run (3)
    CHECK(r.system.controller_status(r.scene, r.root).previous_state == "Fall");
    r.system.update(r.scene, 0.25f);
    CHECK(near(r.x(), 3.0f));
    CHECK(r.system.controller_status(r.scene, r.root).previous_state.empty());
    // Any-state transition wins from anywhere, and does not re-enter itself.
    r.system.set_parameter(r.scene, r.root, "dead", 1);
    r.system.update(r.scene, 0.1f);
    CHECK(r.state() == "Dead");
    r.system.update(r.scene, 0.1f);
    CHECK(r.system.stats().transitions == 0);
    // Unknown parameters and type mismatches are rejected.
    CHECK_FALSE(r.system.set_parameter(r.scene, r.root, "nope", 1));
    CHECK_FALSE(r.system.set_trigger(r.scene, r.root, "speed"));
    CHECK_FALSE(r.system.parameter(r.scene, r.root, "nope").has_value());
}

TEST_CASE("a transition fired mid-fade blends from a frozen pose without a pop") {
    Rig r(R"({
      "parameters": [ { "name": "go", "type": "int" } ],
      "states": [ { "name": "A", "clip": "Idle" }, { "name": "B", "clip": "Run" }, { "name": "C", "clip": "Jump" } ],
      "transitions": [
        { "from": "A", "to": "B", "duration": 1, "conditions": [ { "parameter": "go", "op": "==", "value": 1 } ] },
        { "from": "B", "to": "C", "duration": 1, "conditions": [ { "parameter": "go", "op": "==", "value": 2 } ] } ]
    })");
    r.system.update(r.scene, 0);
    r.system.set_parameter(r.scene, r.root, "go", 1);
    r.system.update(r.scene, 0);
    r.system.update(r.scene, 0.5f);
    CHECK(near(r.x(), 1.5f)); // A (0) -> B (3) halfway
    r.system.set_parameter(r.scene, r.root, "go", 2);
    r.system.update(r.scene, 0); // B -> C fires mid-fade: source frozen at 1.5
    CHECK(r.state() == "C");
    CHECK(near(r.x(), 1.5f));
    r.system.update(r.scene, 0.5f);
    CHECK(near(r.x(), 1.5f + (10.0f - 1.5f) * 0.5f));
    r.system.update(r.scene, 0.5f);
    CHECK(near(r.x(), 10.0f));
}

TEST_CASE("play() and the clip field enter states; looping exit time uses the cycle fraction") {
    Rig r(R"({
      "states": [ { "name": "A", "clip": "Walk" }, { "name": "B", "clip": "Run", "speed": 2 } ],
      "transitions": [ { "from": "A", "to": "B", "duration": 0, "exit_time": 0.75 } ]
    })");
    r.system.update(r.scene, 0);
    CHECK(r.state() == "A");
    r.system.update(r.scene, 1.0f); // Walk is 2 s: 0.5 of the cycle
    CHECK(r.state() == "A");
    r.system.update(r.scene, 0.6f); // 0.8 >= 0.75
    CHECK(r.state() == "B");
    CHECK(near(r.system.status(r.scene, r.root).duration, 1.0f));
    CHECK(r.system.play(r.scene, r.root, "A", 0));
    CHECK(r.state() == "A");
    CHECK_FALSE(r.system.play(r.scene, r.root, "Nope"));
    r.a().clip = "B"; // writing the field enters the state too
    r.system.update(r.scene, 0);
    CHECK(r.state() == "B");
    r.a().clip = "Garbage"; // unknown names snap back to the current state
    r.system.update(r.scene, 0);
    CHECK(r.a().clip == "B");
    // State speed 2: Run (1 s) completes half a cycle in 0.25 s.
    r.system.update(r.scene, 0.25f);
    CHECK(near(r.system.status(r.scene, r.root).normalized_time, 0.5f));
}

TEST_CASE("a looping exit time below 1 fires only on the frame its cycle point is crossed") {
    Rig r(R"({
      "parameters": [ { "name": "ok", "type": "bool" } ],
      "states": [ { "name": "A", "clip": "Idle" }, { "name": "B", "clip": "Run" } ],
      "transitions": [ { "from": "A", "to": "B", "duration": 0, "exit_time": 0.5,
                         "conditions": [ { "parameter": "ok", "op": "true" } ] } ]
    })");
    r.system.update(r.scene, 0);
    r.system.update(r.scene, 0.6f); // crosses 0.5 while "ok" is false
    CHECK(r.state() == "A");
    r.system.set_parameter(r.scene, r.root, "ok", 1);
    r.system.update(r.scene, 0.2f); // 0.8: past the point, but not crossing it
    CHECK(r.state() == "A");
    r.system.update(r.scene, 0.5f); // 1.3: still before the next cycle's 0.5
    CHECK(r.state() == "A");
    r.system.update(r.scene, 0.3f); // 1.6 crosses 1.5
    CHECK(r.state() == "B");
}

TEST_CASE("missing clips use the rest pose; load errors fall back to clip playback") {
    Rig r(R"({ "states": [ { "name": "A", "clip": "NotInModel" } ] })");
    r.system.update(r.scene, 0.1f);
    CHECK(r.state() == "A");
    const ControllerStatus cs = r.system.controller_status(r.scene, r.root);
    REQUIRE(cs.weights.size() == 1);
    CHECK(cs.weights[0].missing);

    Rig bad(R"({ "states": [] })");
    bad.a().clip = "Run";
    bad.system.update(bad.scene, 0);
    CHECK_FALSE(bad.system.status(bad.scene, bad.root).controlled);
    CHECK(near(bad.x(), 3.0f)); // plain clip playback still works
    CHECK(bad.system.stats().controller_errors == 1);
    const ControllerStatus bs = bad.system.controller_status(bad.scene, bad.root);
    CHECK_FALSE(bs.valid);
    CHECK(bs.error.find("at least one state") != std::string::npos);
    bad.system.update(bad.scene, 0);
    CHECK(bad.loads == 1); // the failure is cached, not re-read every frame
    // play() also falls back to clips (not states) when the controller did not load.
    CHECK(bad.system.play(bad.scene, bad.root, "Idle", 0.0f));
    bad.system.update(bad.scene, 0);
    CHECK(near(bad.x(), 0.0f));
    CHECK_FALSE(bad.system.play(bad.scene, bad.root, "NoSuchClip"));
}

TEST_CASE("reload_controllers re-reads the asset and keeps state and parameter values") {
    Rig r;
    r.system.update(r.scene, 0);
    r.system.set_parameter(r.scene, r.root, "speed", 1.5f);
    r.system.update(r.scene, 0);
    CHECK(near(r.x(), 1.0f));
    std::string doc = kController;
    doc.replace(doc.find("\"at\": 1.5"), 9, "\"at\": 3.0");
    r.files["anim/player.vanim"] = doc;
    r.system.reload_controllers();
    r.system.update(r.scene, 0);
    CHECK(r.loads == 2);
    CHECK(r.state() == "Locomotion");
    CHECK(near(*r.system.parameter(r.scene, r.root, "speed"), 1.5f));
    CHECK(near(r.x(), 0.5f)); // Idle (0) .. Walk (1) now spans 0..3
    // Removing the controller returns to plain clip playback.
    r.a().controller.clear();
    r.a().clip = "Jump";
    r.system.update(r.scene, 0);
    CHECK_FALSE(r.system.status(r.scene, r.root).controlled);
    CHECK(near(r.x(), 10.0f));
}

TEST_CASE("controller directory loader stays inside the assets folder") {
    const ControllerLoader load = directory_loader("/nonexistent-assets-root");
    CHECK_FALSE(load("../etc/passwd"));
    CHECK_FALSE(load("/abs/path.vanim"));
    CHECK_FALSE(load("missing.vanim"));
}

TEST_CASE("update_entity advances one Animator and leaves the others alone") {
    Rig r;
    const Entity other = r.scene.create("Other");
    const Entity other_hips = r.scene.create("Hips", other);
    auto& oa = r.scene.world().add<AnimatorComponent>(other);
    oa.model = r.model;
    oa.clip = "Run";
    REQUIRE(r.system.set_parameter(r.scene, r.root, "speed", 9.0f));
    CHECK(r.system.update_entity(r.scene, r.root, 0.25f));
    CHECK(near(r.x(), 3.0f)); // the previewed character ran
    CHECK(near(r.system.status(r.scene, r.root).normalized_time, 0.25f));
    CHECK(near(r.scene.world().get<scene::Transform>(other_hips)->position.x, 0.0f)); // untouched
    CHECK(r.system.stats().animators == 1);
    CHECK(r.system.stats().controlled == 1);
    CHECK_FALSE(r.system.update_entity(r.scene, r.hips, 0.1f)); // no Animator
}

namespace {

// A root clip that walks +Z at `speed` m/s for `duration` seconds (and bobs in Y), optionally
// turning `turn` radians about Y over the clip.
Clip walk_clip(std::string name, float duration, float speed, float turn = 0) {
    Clip c{.name = std::move(name), .duration = duration};
    Channel t{.node = 0, .path = Path::Translation, .interpolation = Interpolation::Linear,
              .times = {0, duration * 0.5f, duration}};
    t.values = {0, 1, 0, 0, 1.2f, speed * duration * 0.5f, 0, 1, speed * duration};
    c.channels.push_back(std::move(t));
    if (turn != 0) {
        Channel r{.node = 0, .path = Path::Rotation, .interpolation = Interpolation::Linear, .times = {0, duration}};
        const math::Quat q = math::Quat::axis_angle({0, 1, 0}, turn);
        r.values = {0, 0, 0, 1, q.x, q.y, q.z, q.w};
        c.channels.push_back(std::move(r));
    }
    return c;
}

constexpr const char* kRootController = R"({
  "version": 1,
  "parameters": [ { "name": "speed", "type": "float" }, { "name": "turn", "type": "trigger" } ],
  "default_state": "Move",
  "root_node": "Hips",
  "states": [
    { "name": "Move", "root_motion": true, "blend1d": { "parameter": "speed",
        "points": [ { "clip": "Idle", "at": 0 }, { "clip": "Walk", "at": 2 } ] } },
    { "name": "InPlace", "clip": "Walk" },
    { "name": "Turn", "clip": "Turn", "root_motion": true, "loop": false }
  ],
  "transitions": [ { "from": "*", "to": "Turn", "duration": 0, "conditions": [ { "parameter": "turn" } ] } ]
})";

struct RootRig {
    Scene scene;
    Entity root, hips;
    Uuid model = Uuid::generate();
    AnimationSystem system{AnimationSettings{.controllers = [](std::string_view) -> Result<std::string> {
        return std::string(kRootController);
    }}};
    RootRig() {
        auto set = std::make_shared<AnimationSet>();
        set->nodes.push_back({.name = "Hips", .parent = -1, .rest = {.translation = {0, 1, 0}}});
        set->clips.push_back(constant_clip("Idle", 1.0f, 0.0f));
        set->clips.push_back(walk_clip("Walk", 1.0f, 2.0f));
        set->clips.push_back(walk_clip("Turn", 1.0f, 0.0f, 1.5707963f));
        // Idle holds Hips at x = 0 but lacks y; give it the rest height via the constant clip.
        set->clips[0].channels[0].values = {0, 1, 0, 0, 1, 0};
        system.add_set(model, set);
        root = scene.create("Character");
        hips = scene.create("Hips", root);
        auto& a = scene.world().add<AnimatorComponent>(root);
        a.model = model;
        a.controller = "anim/root.vanim";
        a.crossfade = 0;
    }
    scene::Transform& tf(Entity e) { return *scene.world().get<scene::Transform>(e); }
    void run(float seconds, float dt = 0.05f) {
        for (float t = 0; t < seconds - 1e-4f; t += dt) system.update(scene, dt);
    }
};

} // namespace

TEST_CASE("controller root_motion and root_node parse, validate and round-trip") {
    auto c = Controller::parse(kRootController);
    REQUIRE(c);
    CHECK((*c)->root_node == "Hips");
    CHECK((*c)->states[0].root_motion);
    CHECK_FALSE((*c)->states[1].root_motion);
    auto again = Controller::from_json((*c)->to_json());
    REQUIRE(again);
    CHECK((*again)->root_node == "Hips");
    CHECK((*again)->states == (*c)->states);
    std::string bad = kRootController;
    bad.replace(bad.find("\"root_motion\": true"), 19, "\"root_motion\": 1   ");
    auto e = Controller::parse(bad);
    REQUIRE_FALSE(e);
    CHECK(e.error().message.find("root_motion") != std::string::npos);
    std::string bad_node = kRootController;
    bad_node.replace(bad_node.find("\"Hips\""), 6, "\"\"    ");
    CHECK_FALSE(Controller::parse(bad_node));
}

TEST_CASE("root motion moves the Animator entity and keeps the root node in place") {
    RootRig rig;
    REQUIRE(rig.system.set_parameter(rig.scene, rig.root, "speed", 2.0f)); // pure Walk: 2 m/s
    rig.system.update(rig.scene, 0.0f);
    rig.run(1.5f);
    // 1.5 s at 2 m/s across a loop boundary, with no pop at the wrap.
    CHECK(near(rig.tf(rig.root).position.z, 3.0f, 1e-3f));
    CHECK(near(rig.tf(rig.root).position.x, 0.0f));
    CHECK(near(rig.tf(rig.root).position.y, 0.0f)); // vertical motion stays in the pose
    CHECK(near(rig.tf(rig.hips).position.z, 0.0f));  // pinned horizontally
    CHECK(near(rig.tf(rig.hips).position.y, 1.2f, 1e-3f)); // the bob at mid-cycle stays
    const RootMotion m = rig.system.root_motion(rig.scene, rig.root);
    CHECK(m.active);
    CHECK(m.applied);
    CHECK(near(m.position.z, 0.1f, 1e-3f));
    const auto st = rig.system.controller_status(rig.scene, rig.root);
    CHECK(st.root_node == "Hips");
    REQUIRE(st.root_motion_states.size() == 3);
    CHECK(st.root_motion_states[0] == 1);

    SUBCASE("half speed blends the travel with the idle clip") {
        REQUIRE(rig.system.set_parameter(rig.scene, rig.root, "speed", 1.0f));
        const float z0 = rig.tf(rig.root).position.z;
        rig.run(1.0f);
        // Weights 0.5/0.5; the phase-synced cycle lasts 1 s, so Walk contributes half of 2 m.
        CHECK(near(rig.tf(rig.root).position.z - z0, 1.0f, 1e-3f));
    }
    SUBCASE("states without root_motion animate the root in place of the entity") {
        REQUIRE(rig.system.play(rig.scene, rig.root, "InPlace", 0.0f));
        const float z0 = rig.tf(rig.root).position.z;
        rig.run(0.25f);
        CHECK(near(rig.tf(rig.root).position.z, z0));
        CHECK(rig.tf(rig.hips).position.z > 0.4f);
        CHECK_FALSE(rig.system.root_motion(rig.scene, rig.root).active);
    }
    SUBCASE("the entity's rotation and scale orient the travel") {
        rig.tf(rig.root).rotation = math::Quat::axis_angle({0, 1, 0}, 1.5707963f); // facing +X
        rig.tf(rig.root).scale = {2, 2, 2};
        const math::Vec3 p0 = rig.tf(rig.root).position;
        rig.run(0.5f);
        CHECK(near(rig.tf(rig.root).position.x - p0.x, 2.0f, 2e-3f)); // 0.5 s * 2 m/s * scale 2
        CHECK(near(rig.tf(rig.root).position.z - p0.z, 0.0f, 2e-3f));
    }
    SUBCASE("apply_root_motion off only reports the motion") {
        rig.scene.world().get<AnimatorComponent>(rig.root)->apply_root_motion = false;
        const float z0 = rig.tf(rig.root).position.z;
        rig.run(0.5f);
        CHECK(near(rig.tf(rig.root).position.z, z0));
        const RootMotion r = rig.system.root_motion(rig.scene, rig.root);
        CHECK(r.active);
        CHECK_FALSE(r.applied);
        CHECK(near(r.position.z, 0.1f, 1e-3f));
        CHECK(near(rig.tf(rig.hips).position.z, 0.0f));
    }
    SUBCASE("a turning clip yaws the entity and the hips keep their start heading") {
        REQUIRE(rig.system.set_trigger(rig.scene, rig.root, "turn"));
        rig.run(1.2f); // one-shot: stops turning at the end
        const math::Vec3 fwd = rig.tf(rig.root).rotation.rotate({0, 0, 1});
        CHECK(near(fwd.x, 1.0f, 2e-3f));
        CHECK(near(fwd.z, 0.0f, 2e-3f));
        const math::Vec3 hips_fwd = rig.tf(rig.hips).rotation.rotate({0, 0, 1});
        CHECK(near(hips_fwd.z, 1.0f, 2e-3f));
        CHECK(rig.system.root_motion(rig.scene, rig.root).active); // still in Turn
        CHECK(near(rig.system.root_motion(rig.scene, rig.root).yaw, 0.0f));      // finished: no more turn
    }
}

TEST_CASE("twist drivers: a forearm twist joint shares the wrist roll, parent sources counter-rotate (ADR-0182)") {
    // Arm along +X: LowerArm > {Twist (mid forearm), Hand (wrist)}; UpperTwist under LowerArm too
    // as a parent-sourced driver of a child joint.
    auto set = std::make_shared<AnimationSet>();
    set->nodes.push_back({.name = "LowerArm", .parent = -1});
    set->nodes.push_back({.name = "Twist", .parent = 0, .rest = {.translation = {0.12f, 0, 0}}});
    set->nodes.push_back({.name = "Hand", .parent = 0, .rest = {.translation = {0.25f, 0, 0}}});
    set->nodes.push_back({.name = "Child", .parent = 0, .rest = {.translation = {0.0f, 0, 0.3f}}});
    // Hand: roll 80 degrees about the forearm (+X) and bend 30 degrees about Z (a pure swing).
    const math::Quat roll = math::Quat::axis_angle({1, 0, 0}, math::radians(80.0f));
    const math::Quat bend = math::Quat::axis_angle({0, 0, 1}, math::radians(30.0f));
    const math::Quat hand = bend * roll;
    // LowerArm itself rolls 40 degrees about +Z, the Child's bone axis.
    const math::Quat arm = math::Quat::axis_angle({0, 0, 1}, math::radians(40.0f));
    Clip c{.name = "Pose", .duration = 1.0f};
    Channel h{.node = 2, .path = Path::Rotation, .interpolation = Interpolation::Linear, .times = {0, 1}};
    h.values = {hand.x, hand.y, hand.z, hand.w, hand.x, hand.y, hand.z, hand.w};
    Channel l{.node = 0, .path = Path::Rotation, .interpolation = Interpolation::Linear, .times = {0, 1}};
    l.values = {arm.x, arm.y, arm.z, arm.w, arm.x, arm.y, arm.z, arm.w};
    c.channels = {h, l};
    set->clips.push_back(std::move(c));

    const std::string doc = R"({ "states": [ { "name": "Pose", "clip": "Pose" } ],
      "twist": [ { "joint": "Twist", "source": "Hand", "weight": 0.5 },
                 { "joint": "Child", "source": "LowerArm", "weight": -0.25 } ] })";
    auto parsed = Controller::parse(doc, "t.vanim");
    REQUIRE(parsed.ok());
    REQUIRE((*parsed)->twists.size() == 2);
    auto again = Controller::from_json((*parsed)->to_json());
    REQUIRE(again.ok());
    CHECK((*again)->twists == (*parsed)->twists);

    Scene scene;
    Uuid model = Uuid::generate();
    AnimationSystem system{AnimationSettings{.controllers = [&](std::string_view) -> Result<std::string> { return doc; }}};
    system.add_set(model, set);
    const Entity root = scene.create("Character");
    const Entity la = scene.create("LowerArm", root);
    const Entity tw = scene.create("Twist", la);
    scene.create("Hand", la);
    const Entity child = scene.create("Child", la);
    auto& a = scene.world().add<AnimatorComponent>(root);
    a.model = model;
    a.controller = "c.vanim";
    system.update(scene, 0.1f);
    // Half of the 80 degree roll, none of the bend.
    const math::Quat got = scene.world().get<scene::Transform>(tw)->rotation;
    const math::Quat want = math::Quat::axis_angle({1, 0, 0}, math::radians(40.0f));
    CHECK(std::abs(got.x * want.x + got.y * want.y + got.z * want.z + got.w * want.w) == doctest::Approx(1.0f).epsilon(1e-4));
    // Child counter-rotates a quarter of LowerArm's 40 degree roll about its +Z axis.
    const math::Quat gc = scene.world().get<scene::Transform>(child)->rotation;
    const math::Quat wc = math::Quat::axis_angle({0, 0, 1}, math::radians(-10.0f));
    CHECK(std::abs(gc.x * wc.x + gc.y * wc.y + gc.z * wc.z + gc.w * wc.w) == doctest::Approx(1.0f).epsilon(1e-4));

    const std::string same = parse_error(R"({ "states": [ { "name": "S", "clip": "Idle" } ], "twist": [ { "joint": "A", "source": "A" } ] })");
    INFO(same);
    CHECK(same.find("must differ") != std::string::npos);
    const std::string range =
        parse_error(R"({ "states": [ { "name": "S", "clip": "Idle" } ], "twist": [ { "joint": "A", "source": "B", "weight": 2 } ] })");
    INFO(range);
    CHECK(range.find("[-1, 1]") != std::string::npos);
}
