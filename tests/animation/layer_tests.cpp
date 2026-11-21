// SPDX-License-Identifier: Apache-2.0
// Animation controller layers (ADR-0074): override and additive layers, bone masks with
// exclusions, empty states, layer weights (authored, parameter-driven, runtime) and status.
#include "victus/animation/animation.hpp"
#include "victus/core/json.hpp"

#include <doctest.h>

#include <cmath>
#include <map>
#include <numbers>

using namespace victus;
using namespace victus::animation;
using math::Quat;
using math::Vec3;
using scene::Entity;
using scene::Scene;

namespace {

bool near(float a, float b, float eps = 1e-4f) { return std::abs(a - b) <= eps; }

enum Node : std::uint32_t { kHips, kSpine, kArm, kLeg };

Channel tx(std::uint32_t node, float duration, float from, float to) {
    Channel ch{.node = node, .path = Path::Translation, .interpolation = Interpolation::Linear, .times = {0, duration}};
    ch.values = {from, 0, 0, to, 0, 0};
    return ch;
}

Channel rot(std::uint32_t node, float duration, Quat from, Quat to) {
    Channel ch{.node = node, .path = Path::Rotation, .interpolation = Interpolation::Linear, .times = {0, duration}};
    ch.values = {from.x, from.y, from.z, from.w, to.x, to.y, to.z, to.w};
    return ch;
}

const Quat kRestSpine = Quat::axis_angle({0, 0, 1}, std::numbers::pi_v<float> / 6.0f);

// Hips -> Spine -> Arm, Hips -> Leg. Translation x is the probe value.
std::shared_ptr<AnimationSet> make_set() {
    auto set = std::make_shared<AnimationSet>();
    set->nodes.push_back({.name = "Hips", .parent = -1, .rest = {}});
    set->nodes.push_back({.name = "Spine", .parent = 0, .rest = {.translation = {-1, 0, 0}, .rotation = kRestSpine}});
    set->nodes.push_back({.name = "Arm", .parent = 1, .rest = {.translation = {-1, 0, 0}}});
    set->nodes.push_back({.name = "Leg", .parent = 0, .rest = {.translation = {-1, 0, 0}}});
    Clip walk{.name = "Walk", .duration = 1};
    for (std::uint32_t n : {kHips, kSpine, kArm, kLeg}) walk.channels.push_back(tx(n, 1, 1, 1));
    Clip aim{.name = "Aim", .duration = 1};
    for (std::uint32_t n : {kSpine, kArm, kLeg}) aim.channels.push_back(tx(n, 1, 5, 5));
    Clip wave{.name = "Wave", .duration = 1}; // Arm.x 0 -> 2; Spine turns 0 -> 90 degrees about Y
    wave.channels.push_back(tx(kArm, 1, 0, 2));
    wave.channels.push_back(rot(kSpine, 1, Quat{}, Quat::axis_angle({0, 1, 0}, std::numbers::pi_v<float> / 2.0f)));
    Clip lean{.name = "Lean", .duration = 1}; // constant Arm.x = 3 (additive against the rest pose: +4)
    lean.channels.push_back(tx(kArm, 1, 3, 3));
    for (Clip* c : {&walk, &aim, &wave, &lean}) set->clips.push_back(std::move(*c));
    return set;
}

constexpr const char* kLayered = R"({
  "version": 1,
  "parameters": [ { "name": "aim", "type": "float", "default": 1 },
                  { "name": "shoot", "type": "trigger" } ],
  "states": [ { "name": "Walk", "clip": "Walk" } ],
  "layers": [
    { "name": "UpperBody", "mask": ["Spine"], "weight_parameter": "aim",
      "default_state": "None",
      "states": [ { "name": "None" }, { "name": "Aim", "clip": "Aim" } ],
      "transitions": [ { "from": "None", "to": "Aim", "duration": 0.5, "conditions": [ { "parameter": "shoot" } ] } ] }
  ]
})";

struct Rig {
    Scene scene;
    Entity root, hips, spine, arm, leg;
    Uuid model = Uuid::generate();
    std::map<std::string, std::string> files;
    AnimationSystem system{AnimationSettings{.controllers = [this](std::string_view p) -> Result<std::string> {
        const auto it = files.find(std::string(p));
        if (it == files.end()) return make_error(ErrorCode::NotFound, "no such file");
        return it->second;
    }}};
    explicit Rig(const std::string& doc) {
        files["anim/c.vanim"] = doc;
        system.add_set(model, make_set());
        root = scene.create("Character");
        hips = scene.create("Hips", root);
        spine = scene.create("Spine", hips);
        arm = scene.create("Arm", spine);
        leg = scene.create("Leg", hips);
        auto& a = scene.world().add<AnimatorComponent>(root);
        a.model = model;
        a.controller = "anim/c.vanim";
        a.crossfade = 0;
    }
    scene::Transform& t(Entity e) { return *scene.world().get<scene::Transform>(e); }
    float x(Entity e) { return t(e).position.x; }
    std::string state() { return system.status(scene, root).state; }
};

std::string layer_doc(const std::string& layer, const std::string& base_clip = "Walk") {
    return R"({ "parameters": [ { "name": "w", "type": "float", "default": 1 } ],
      "states": [ { "name": "Base", "clip": ")" +
           base_clip + R"(" } ], "layers": [ )" + layer + " ] }";
}

std::string parse_error(const std::string& doc) {
    auto c = Controller::parse(doc, "t.vanim");
    return c ? std::string() : c.error().message;
}

float angle_between(Quat a, Quat b) {
    const float d = std::min(1.0f, std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w));
    return 2.0f * std::acos(d);
}

} // namespace

TEST_CASE("layers: parse, validate with nested element names and round-trip") {
    auto c = Controller::parse(kLayered, "c.vanim");
    REQUIRE(c);
    const Controller& ctrl = **c;
    REQUIRE(ctrl.layers.size() == 1);
    const ControllerLayer& l = ctrl.layers[0];
    CHECK(l.name == "UpperBody");
    CHECK(l.blend == LayerBlend::Override);
    CHECK(l.mask == std::vector<std::string>{"Spine"});
    REQUIRE(l.machine);
    CHECK(l.machine->states.size() == 2);
    CHECK(l.machine->states[0].clip.empty()); // empty state
    CHECK(l.machine->default_state == 0);
    CHECK(l.machine->parameters == ctrl.parameters); // shared
    CHECK(ctrl.find_layer("UpperBody") == 0);
    CHECK(ctrl.find_layer("Legs") == -1);
    CHECK(ctrl.clip_names() == std::vector<std::string>{"Walk", "Aim"});

    auto again = Controller::from_json(ctrl.to_json(), "c.vanim");
    REQUIRE(again);
    CHECK(json::stringify((*again)->to_json()) == json::stringify(ctrl.to_json()));
    CHECK((*again)->layers[0].machine->states == l.machine->states);

    auto additive = Controller::parse(layer_doc(R"({ "name": "A", "blend": "additive", "reference": "rest", "weight": 0.25,
        "exclude": ["Leg"], "states": [ { "name": "S", "clip": "Wave" } ] })"));
    REQUIRE(additive);
    CHECK((*additive)->layers[0].blend == LayerBlend::Additive);
    CHECK((*additive)->layers[0].reference == AdditiveReference::Rest);
    CHECK((*additive)->layers[0].weight == 0.25f);
    CHECK((*additive)->layers[0].exclude == std::vector<std::string>{"Leg"});
    auto rt = Controller::from_json((*additive)->to_json());
    REQUIRE(rt);
    CHECK(json::stringify((*rt)->to_json()) == json::stringify((*additive)->to_json()));

    const std::string s = R"("states": [ { "name": "S", "clip": "Aim" } ])";
    CHECK(parse_error(layer_doc(R"({ "name": "L", "blend": "multiply", )" + s + "}")).find("layers[0]: unknown blend") !=
          std::string::npos);
    CHECK(parse_error(layer_doc(R"({ "name": "L", "weight": 2, )" + s + "}")).find("\"weight\" must be in [0, 1]") !=
          std::string::npos);
    CHECK(parse_error(layer_doc(R"({ "name": "L", "weight_parameter": "nope", )" + s + "}")).find("unknown parameter") !=
          std::string::npos);
    CHECK(parse_error(layer_doc(R"({ "name": "L", "reference": "rest", )" + s + "}")).find("only applies to additive") !=
          std::string::npos);
    CHECK(parse_error(layer_doc(R"({ "name": "L", "mask": [""], )" + s + "}")).find("\"mask\"[0]") != std::string::npos);
    CHECK(parse_error(layer_doc(R"({ "name": "L", "parameters": [], )" + s + "}")).find("not allowed in a layer") !=
          std::string::npos);
    CHECK(parse_error(layer_doc(R"({ "name": "L", "states": [ { "name": "S", "clip": "Aim", "root_motion": true } ] })"))
              .find("layers[0].states[0]: root motion is only taken from the base layer") != std::string::npos);
    CHECK(parse_error(layer_doc(R"({ "name": "L", "states": [ { "name": "S" } ],
        "transitions": [ { "from": "S", "to": "T", "duration": 0, "exit_time": 1 } ] })"))
              .find("layers[0].transitions[0]: unknown state \"T\"") != std::string::npos);
    CHECK(parse_error(layer_doc(R"({ "name": "L", )" + s + R"( }, { "name": "L", )" + s + "}"))
              .find("duplicate layer \"L\"") != std::string::npos);
    CHECK(parse_error(layer_doc(R"({ "name": "L", "states": [] })")).find("at least one state") != std::string::npos);
    // Empty states are only allowed inside layers.
    CHECK(parse_error(R"({ "states": [ { "name": "S" } ] })").find("exactly one of") != std::string::npos);
}

TEST_CASE("layers: an override layer replaces only its masked subtree, by weight") {
    Rig r(layer_doc(R"({ "name": "Upper", "mask": ["Spine"], "weight_parameter": "w",
        "states": [ { "name": "Aim", "clip": "Aim" } ] })"));
    r.system.update(r.scene, 0);
    CHECK(near(r.x(r.hips), 1));
    CHECK(near(r.x(r.leg), 1)); // outside the mask: Walk
    CHECK(near(r.x(r.spine), 5));
    CHECK(near(r.x(r.arm), 5)); // descendant of Spine
    REQUIRE(r.system.set_parameter(r.scene, r.root, "w", 0.5f));
    r.system.update(r.scene, 0.1f);
    CHECK(near(r.x(r.spine), 3));
    CHECK(near(r.x(r.leg), 1));
    REQUIRE(r.system.set_parameter(r.scene, r.root, "w", 1));
    REQUIRE(r.system.set_layer_weight(r.scene, r.root, "Upper", 0.25f));
    CHECK(near(*r.system.layer_weight(r.scene, r.root, "Upper"), 0.25f));
    r.system.update(r.scene, 0.1f);
    CHECK(near(r.x(r.arm), 2)); // 1 + 0.25 * (5 - 1)
    CHECK_FALSE(r.system.set_layer_weight(r.scene, r.root, "Nope", 1));
    CHECK_FALSE(r.system.layer_weight(r.scene, r.root, "Nope"));
    r.system.set_layer_weight(r.scene, r.root, "Upper", 0);
    r.system.update(r.scene, 0.1f);
    CHECK(near(r.x(r.spine), 1));

    const ControllerStatus cs = r.system.controller_status(r.scene, r.root);
    REQUIRE(cs.layers.size() == 1);
    CHECK(cs.layers[0].name == "Upper");
    CHECK(cs.layers[0].state == "Aim");
    CHECK(cs.layers[0].affected_nodes == 2); // Spine and Arm (Aim also animates Leg, masked out)
    CHECK(cs.layers[0].weight == 0.0f);
}

TEST_CASE("layers: exclude removes a subtree; an empty mask covers every animated node") {
    Rig r(layer_doc(R"({ "name": "Upper", "mask": ["Spine"], "exclude": ["Arm"], "states": [ { "name": "Aim", "clip": "Aim" } ] })"));
    r.system.update(r.scene, 0);
    CHECK(near(r.x(r.spine), 5));
    CHECK(near(r.x(r.arm), 1));
    Rig all(layer_doc(R"({ "name": "All", "states": [ { "name": "Aim", "clip": "Aim" } ] })"));
    all.system.update(all.scene, 0);
    CHECK(near(all.x(all.hips), 1)); // Aim does not animate Hips: the base pose stays
    CHECK(near(all.x(all.leg), 5));
    CHECK(near(all.x(all.arm), 5));
    Rig unknown(layer_doc(R"({ "name": "U", "mask": ["Tail"], "states": [ { "name": "Aim", "clip": "Aim" } ] })"));
    unknown.system.update(unknown.scene, 0);
    CHECK(near(unknown.x(unknown.spine), 1)); // the mask names no node of this model: no influence
}

TEST_CASE("layers: empty states have no influence and layer transitions cross-fade from the base pose") {
    Rig r(kLayered);
    r.system.update(r.scene, 0);
    CHECK(near(r.x(r.spine), 1)); // UpperBody in None
    CHECK(r.system.controller_status(r.scene, r.root).layers[0].state == "None");
    REQUIRE(r.system.set_trigger(r.scene, r.root, "shoot"));
    r.system.update(r.scene, 0); // fires; the fade starts
    CHECK(r.system.controller_status(r.scene, r.root).layers[0].state == "Aim");
    CHECK(r.system.parameter(r.scene, r.root, "shoot") == 0.0f); // consumed
    r.system.update(r.scene, 0.25f);
    CHECK(near(r.x(r.spine), 3)); // halfway from base (1) to Aim (5)
    CHECK(near(r.x(r.leg), 1));
    const ControllerStatus cs = r.system.controller_status(r.scene, r.root);
    CHECK(cs.layers[0].previous_state == "None");
    CHECK(near(cs.layers[0].blend_progress, 0.5f));
    r.system.update(r.scene, 0.5f);
    CHECK(near(r.x(r.spine), 5));
    CHECK(r.state() == "Walk"); // base layer unaffected
}

TEST_CASE("layers: additive layers add the difference from the first frame or the rest pose") {
    Rig r(layer_doc(R"({ "name": "Add", "blend": "additive", "weight_parameter": "w",
        "states": [ { "name": "Wave", "clip": "Wave" } ] })"));
    r.system.update(r.scene, 0);
    CHECK(near(r.x(r.arm), 1)); // first frame: no difference
    r.system.update(r.scene, 0.5f);
    CHECK(near(r.x(r.arm), 2)); // Walk 1 + (1 - 0)
    // Spine: base rotation (rest, not animated by Walk) followed by +45 degrees about Y.
    const Quat expect = kRestSpine * Quat::axis_angle({0, 1, 0}, std::numbers::pi_v<float> / 4.0f);
    CHECK(angle_between(r.t(r.spine).rotation, expect) < 1e-3f);
    CHECK(near(r.x(r.leg), 1)); // not animated by the layer
    r.system.set_parameter(r.scene, r.root, "w", 0.5f);
    r.system.update(r.scene, 0);
    CHECK(near(r.x(r.arm), 1.5f));
    const Quat half = kRestSpine * Quat::axis_angle({0, 1, 0}, std::numbers::pi_v<float> / 8.0f);
    CHECK(angle_between(r.t(r.spine).rotation, half) < 1e-3f);

    Rig rest(layer_doc(R"({ "name": "Add", "blend": "additive", "reference": "rest", "states": [ { "name": "Lean", "clip": "Lean" } ] })"));
    rest.system.update(rest.scene, 0.3f);
    CHECK(near(rest.x(rest.arm), 5)); // Walk 1 + (3 - (-1))
}

TEST_CASE("layers: hot reload keeps runtime layer weights; removing the controller clears layers") {
    Rig r(layer_doc(R"({ "name": "Upper", "mask": ["Spine"], "states": [ { "name": "Aim", "clip": "Aim" } ] })"));
    r.system.update(r.scene, 0);
    r.system.set_layer_weight(r.scene, r.root, "Upper", 0.5f);
    r.files["anim/c.vanim"] =
        layer_doc(R"({ "name": "Upper", "mask": ["Spine"], "weight": 1, "states": [ { "name": "Aim2", "clip": "Aim" } ] })");
    r.system.reload_controllers();
    r.system.update(r.scene, 0);
    CHECK(near(*r.system.layer_weight(r.scene, r.root, "Upper"), 0.5f));
    CHECK(r.system.controller_status(r.scene, r.root).layers[0].state == "Aim2");
    CHECK(near(r.x(r.spine), 3));
    r.scene.world().get<AnimatorComponent>(r.root)->controller.clear();
    r.system.update(r.scene, 0);
    CHECK(r.system.controller_status(r.scene, r.root).layers.empty());
}

TEST_CASE("layers: weighted masks scale the influence of each subtree (ADR-0159)") {
    // Spine at 25 %, Arm (under Spine) at 100 %; Leg is not listed: with only weights, they are the mask.
    Rig r(layer_doc(R"({ "name": "Aim", "mask_weights": { "Spine": 0.25, "Arm": 1 },
        "states": [ { "name": "Aim", "clip": "Aim" } ] })"));
    r.system.update(r.scene, 0);
    CHECK(near(r.x(r.hips), 1));
    CHECK(near(r.x(r.spine), 2)); // 1 + 0.25 * (5 - 1)
    CHECK(near(r.x(r.arm), 5));
    CHECK(near(r.x(r.leg), 1));
    CHECK(r.system.controller_status(r.scene, r.root).layers[0].affected_nodes == 2);
    // The layer weight multiplies the mask weight.
    REQUIRE(r.system.set_layer_weight(r.scene, r.root, "Aim", 0.5f));
    r.system.update(r.scene, 0.1f);
    CHECK(near(r.x(r.spine), 1.5f));
    CHECK(near(r.x(r.arm), 3));

    // Mixed with a plain mask and an exclusion: the nearest listed ancestor decides.
    Rig mixed(layer_doc(R"({ "name": "M", "mask": ["Leg"], "mask_weights": { "Spine": 0.5 },
        "states": [ { "name": "Aim", "clip": "Aim" } ] })"));
    mixed.system.update(mixed.scene, 0);
    CHECK(near(mixed.x(mixed.leg), 5));
    CHECK(near(mixed.x(mixed.spine), 3));
    CHECK(near(mixed.x(mixed.arm), 3)); // inherits Spine's 0.5
    Rig excl(layer_doc(R"({ "name": "E", "exclude": ["Arm"], "mask_weights": { "Spine": 0.5 },
        "states": [ { "name": "Aim", "clip": "Aim" } ] })"));
    excl.system.update(excl.scene, 0);
    CHECK(near(excl.x(excl.spine), 3));
    CHECK(near(excl.x(excl.arm), 1));
    CHECK(near(excl.x(excl.leg), 1)); // weights present: unlisted nodes are outside

    // Additive layers scale the added difference: Lean adds +4 on Arm against the rest pose.
    Rig add(layer_doc(R"({ "name": "A", "blend": "additive", "reference": "rest", "mask_weights": { "Arm": 0.5 },
        "states": [ { "name": "S", "clip": "Lean" } ] })"));
    add.system.update(add.scene, 0);
    CHECK(near(add.x(add.arm), 3)); // 1 + 0.5 * 4

    // Round-trip keeps the order and values; validation names the problem.
    auto c = Controller::parse(layer_doc(R"({ "name": "W", "mask_weights": { "Spine": 0.25, "Arm": 1 },
        "states": [ { "name": "Aim", "clip": "Aim" } ] })"));
    REQUIRE(c);
    const auto& mw = (*c)->layers[0].mask_weights;
    REQUIRE(mw.size() == 2);
    CHECK(mw[0].first == "Spine");
    CHECK(mw[0].second == 0.25f);
    auto rt = Controller::from_json((*c)->to_json());
    REQUIRE(rt);
    CHECK(json::stringify((*rt)->to_json()) == json::stringify((*c)->to_json()));
    CHECK((*rt)->layers[0].mask_weights == mw);
    const std::string s = R"("states": [ { "name": "S", "clip": "Aim" } ])";
    CHECK(parse_error(layer_doc(R"({ "name": "L", "mask_weights": { "Spine": 1.5 }, )" + s + "}"))
              .find("\"mask_weights\".\"Spine\" must be a number from 0 to 1") != std::string::npos);
    CHECK(parse_error(layer_doc(R"({ "name": "L", "mask_weights": ["Spine"], )" + s + "}")).find("must be an object") !=
          std::string::npos);
    CHECK(parse_error(layer_doc(R"({ "name": "L", "mask": ["Spine"], "mask_weights": { "Spine": 0.5 }, )" + s + "}"))
              .find("listed twice") != std::string::npos);
}

TEST_CASE("layers: a synced layer's looping states follow the base layer's phase (ADR-0160)") {
    // Base Wave (1 s) at speed 2 runs two cycles per second; the layer's Lean2 clip is 4 s long.
    // Unsynced, the layer would be at 1/8 of its cycle after 0.25 s; synced it is at the base phase.
    const char* doc = R"({ "states": [ { "name": "Base", "clip": "Wave", "speed": 2 } ],
      "layers": [ { "name": "S", "sync": true, "mask": ["Arm"],
                    "states": [ { "name": "Slow", "clip": "Slow" } ] } ] })";
    auto setup = [](Rig& r) {
        auto set = make_set();
        Clip slow{.name = "Slow", .duration = 4}; // Arm.x 0 -> 8 over 4 s
        slow.channels.push_back(tx(kArm, 4, 0, 8));
        set->clips.push_back(std::move(slow));
        r.system.add_set(r.model, set);
    };
    Rig r(doc);
    setup(r);
    r.system.update(r.scene, 0);
    r.system.update(r.scene, 0.2f); // base phase 0.4
    const ControllerStatus cs = r.system.controller_status(r.scene, r.root);
    REQUIRE(cs.layers.size() == 1);
    CHECK(near(cs.normalized_time, 0.4f, 1e-3f));
    CHECK(near(cs.layers[0].normalized_time, cs.normalized_time, 1e-4f));
    CHECK(near(r.x(r.arm), 0.4f * 8.0f, 1e-3f));

    std::string unsynced = doc;
    unsynced.replace(unsynced.find("\"sync\": true"), 12, "\"sync\": false");
    Rig u(unsynced);
    setup(u);
    u.system.update(u.scene, 0);
    u.system.update(u.scene, 0.2f);
    CHECK(near(u.system.controller_status(u.scene, u.root).layers[0].normalized_time, 0.05f, 1e-3f)); // 0.2 s of 4 s
    CHECK(near(u.x(u.arm), 0.4f, 1e-3f));

    auto c = Controller::parse(doc);
    REQUIRE(c);
    CHECK((*c)->layers[0].sync);
    auto rt = Controller::from_json((*c)->to_json());
    REQUIRE(rt);
    CHECK((*rt)->layers[0].sync);
    CHECK(parse_error(layer_doc(R"({ "name": "L", "sync": 1, "states": [ { "name": "S", "clip": "Aim" } ] })"))
              .find("\"sync\" must be true or false") != std::string::npos);
}
